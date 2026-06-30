#include "Scene/SceneEventRelay.h"

#include "Util/StringUtil.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/S/Struct.h"
#include "RE/S/StructTypeInfo.h"
#include "RE/V/VirtualMachine.h"

namespace OSF::Scene
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		// Build the OSFTypes:SceneEvent struct payload. Map member name -> slot index by
		// ITERATING varNameIndexMap: the compiler REORDERS struct members (declaration order
		// != slot order — verified in-game), and the map's `find` proved unreliable on this
		// BSFixedString-keyed table (only hash-coincidence members hit, both with string_view
		// and BSFixedString keys). Iterating reads the true (name, index) pairs directly.
		// Case-normalized for safety. Returns false if the struct type isn't loaded.
		bool PackPayload(VM* a_vm, const SceneEvent& a_event, RE::BSScript::Variable& a_out)
		{
			RE::BSTSmartPointer<RE::BSScript::Struct> proxy;
			if (!a_vm->CreateStruct("OSFTypes#SceneEvent", proxy) || !proxy || !proxy->type) {
				REX::WARN("[Scene] OSFTypes:SceneEvent struct type not loaded");
				return false;
			}

			std::unordered_map<std::string, std::uint32_t> index;
			for (const auto& kv : proxy->type->varNameIndexMap) {
				index[Util::ToLower(kv.key.c_str())] = kv.value;
			}

			const auto count = proxy->type->variables.size();
			auto set = [&](const char* a_member, const RE::BSScript::Variable& a_val) {
				const auto it = index.find(Util::ToLower(a_member));
				if (it != index.end() && it->second < count) {
					proxy->variables[it->second] = a_val;
				} else {
					REX::TRACE("[Scene] member '{}' not found in OSFTypes:SceneEvent", a_member);
				}
			};

			RE::BSScript::Variable v;
			v = a_event.scene;                                  set("sceneHandle", v);
			v = a_event.event;                                  set("eventType", v);
			v = RE::BSFixedString(a_event.node.c_str());        set("node", v);
			v = RE::BSFixedString(a_event.edge.c_str());        set("edge", v);
			v = RE::BSFixedString(a_event.cue.c_str());         set("cue", v);
			v = RE::BSFixedString(a_event.actionType.c_str());  set("actionType", v);
			v = RE::BSFixedString(a_event.role.c_str());        set("role", v);
			v = a_event.loopIndex;                              set("loopIndex", v);
			v = a_event.time;                                   set("time", v);
			v = RE::BSFixedString(a_event.anchor.c_str());      set("anchor", v);
			v = a_event.result;                                 set("result", v);
			// actorRef: packed as a real Actor object when the event carries one (currently
			// EVENT_ACTION with a resolved role). Otherwise left None.
			if (a_event.actor) {
				RE::BSScript::Variable actorVar;
				RE::BSScript::detail::PackVariable(actorVar, a_event.actor);
				set("actorRef", actorVar);
			}

			a_out = proxy;
			return true;
		}

		// A one-argument call: args[0] = the struct payload. Capturing the Variable (a
		// copy shares the struct proxy) keeps it alive until the async stack consumes it.
		auto MakeArgs(RE::BSScript::Variable a_payload)
		{
			return [payload = std::move(a_payload)](RE::BSScrapArray<RE::BSScript::Variable>& a_args) -> bool {
				a_args.resize(1);
				a_args[0] = payload;
				return true;
			};
		}
	}

	SceneEventRelay& SceneEventRelay::GetSingleton()
	{
		static SceneEventRelay singleton;
		return singleton;
	}

	std::int32_t SceneEventRelay::AddEntry(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
		RE::BSFixedString a_scriptName, std::string_view a_fn, std::int32_t a_sceneFilter, std::int32_t a_eventMask)
	{
		std::uint16_t slot = 0;
		bool reused = false;
		for (; slot < _slots.size(); slot++) {
			if (_slots[slot].generation == 0) {
				reused = true;
				break;
			}
		}
		if (!reused) {
			if (_slots.size() >= 0xFFFF) {
				REX::ERROR("[Scene] AddEntry: callback table full");
				return 0;
			}
			_slots.emplace_back();
		}

		const std::uint16_t gen = _nextGen++;
		if (_nextGen == 0) {
			_nextGen = 1;  // never hand out generation 0 (the empty-slot marker)
		}

		Entry& e = _slots[slot];
		e.generation = gen;
		e.receiver = a_receiver;
		e.scriptName = a_scriptName;
		e.fn = RE::BSFixedString(std::string(a_fn).c_str());
		e.sceneFilter = a_sceneFilter;
		e.eventMask = (a_eventMask == 0) ? Event::kAll : a_eventMask;

		const std::int32_t token = MakeToken(gen, slot);
		REX::DEBUG("[Scene] registered token {:#010x} -> {}{}(OSFTypes:SceneEvent) (mask {:#x}, scene {})",
			token, a_scriptName.empty() ? "" : std::string(a_scriptName.c_str()) + ".", e.fn.c_str(), e.eventMask, e.sceneFilter);
		return token;
	}

	std::int32_t SceneEventRelay::Register(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, std::string_view a_fn,
		std::int32_t a_sceneFilter, std::int32_t a_eventMask)
	{
		if (!a_receiver.get() || a_fn.empty()) {
			REX::DEBUG("[Scene] Register: null receiver or empty function name");
			return 0;
		}

		std::lock_guard l{ _lock };
		return AddEntry(a_receiver, RE::BSFixedString(), a_fn, a_sceneFilter, a_eventMask);
	}

	std::int32_t SceneEventRelay::RegisterStatic(std::string_view a_script, std::string_view a_fn,
		std::int32_t a_sceneFilter, std::int32_t a_eventMask)
	{
		if (a_script.empty() || a_fn.empty()) {
			REX::DEBUG("[Scene] RegisterStatic: empty script or function name");
			return 0;
		}

		std::lock_guard l{ _lock };
		return AddEntry({}, RE::BSFixedString(std::string(a_script).c_str()), a_fn, a_sceneFilter, a_eventMask);
	}

	bool SceneEventRelay::Unregister(std::int32_t a_token)
	{
		if (a_token == 0) {
			return false;
		}
		const auto slot = static_cast<std::uint16_t>(a_token & 0xFFFF);
		const auto gen = static_cast<std::uint16_t>((a_token >> 16) & 0xFFFF);

		std::lock_guard l{ _lock };
		if (slot >= _slots.size() || _slots[slot].generation != gen) {
			return false;  // stale/invalid token
		}
		_slots[slot] = Entry{};  // generation 0 -> empty; drops the receiver smart pointer
		REX::DEBUG("[Scene] unregistered token {:#010x}", a_token);
		return true;
	}

	void SceneEventRelay::Dispatch(const SceneEvent& a_event)
	{
		// Snapshot matching receivers under the lock; dispatch outside it (the VM call
		// may re-enter us via a callback that (un)registers).
		struct Target
		{
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;    // null when scriptName is set
			RE::BSFixedString                         scriptName;  // set => static (global) dispatch
			RE::BSFixedString                         fn;
		};
		std::vector<Target> targets;
		{
			std::lock_guard l{ _lock };
			for (const auto& e : _slots) {
				if (e.generation == 0 || (!e.receiver && e.scriptName.empty())) {
					continue;
				}
				if ((e.eventMask & a_event.event) == 0) {
					continue;
				}
				if (e.sceneFilter != 0 && e.sceneFilter != a_event.scene) {
					continue;
				}
				targets.emplace_back(e.receiver, e.scriptName, e.fn);
			}
		}
		if (targets.empty()) {
			return;
		}

		auto* vm = VM::GetSingleton();
		if (!vm) {
			REX::WARN("[Scene] Dispatch: no VM");
			return;
		}
		RE::BSScript::Variable payload;
		if (!PackPayload(vm, a_event, payload)) {
			return;
		}

		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
		for (auto& t : targets) {
			if (!t.scriptName.empty()) {
				vm->DispatchStaticCall(t.scriptName, t.fn, MakeArgs(payload), noCallback, 0);
			} else {
				vm->DispatchMethodCall(t.receiver, t.fn, MakeArgs(payload), noCallback, 0);
			}
		}
	}

	bool SceneEventRelay::DispatchStatic(std::string_view a_script, std::string_view a_fn, const SceneEvent& a_event)
	{
		auto* vm = VM::GetSingleton();
		if (!vm) {
			REX::WARN("[Scene] DispatchStatic: no VM");
			return false;
		}
		RE::BSScript::Variable payload;
		if (!PackPayload(vm, a_event, payload)) {
			return false;
		}
		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
		return vm->DispatchStaticCall(
			RE::BSFixedString(std::string(a_script).c_str()),
			RE::BSFixedString(std::string(a_fn).c_str()),
			MakeArgs(payload), noCallback, 0);
	}

	void SceneEventRelay:Clear()
	{
		std::lock_guard l{ _lock };

		// VM is being reset and maybe already freed, so cached receiver pointers are potentially dangling
		// Dont run release, just forget about them instead by overwriting to a null pointer.
		// Expectation is vm owns the objects and manages lifetime/frees so shouldnt be any leaks
		for (auto& e : _slots) {
			std::construct_at(std::addressof(e.receiver));  // overwrite _ptr=null, skip Release
		}
		_slots.clear();

		// _nextGen is intentionally NOT reset here: keeping it monotonic across a clear means a tokenwwwwwwwwwwww minted before a save-load can never validate against a slot reused after the load 
	}
}
