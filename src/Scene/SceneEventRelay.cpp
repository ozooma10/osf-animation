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

		// Build the OSFEvent:SceneEvent struct payload. Map member name -> slot index by
		// ITERATING varNameIndexMap: the compiler REORDERS struct members (declaration order
		// != slot order — verified in-game), and the map's `find` proved unreliable on this
		// BSFixedString-keyed table (only hash-coincidence members hit, both with string_view
		// and BSFixedString keys). Iterating reads the true (name, index) pairs directly.
		// Case-normalized for safety. Returns false if the struct type isn't loaded.
		bool PackPayload(VM* a_vm, const SceneEvent& a_event, RE::BSScript::Variable& a_out)
		{
			RE::BSTSmartPointer<RE::BSScript::Struct> proxy;
			if (!a_vm->CreateStruct("OSFEvent#SceneEvent", proxy) || !proxy || !proxy->type) {
				REX::WARN("SceneEventRelay: OSFEvent:SceneEvent struct type not loaded");
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
					REX::WARN("SceneEventRelay: member '{}' not found in OSFEvent:SceneEvent", a_member);
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
			// actorRef: packed as a real Actor object via CLSF's handle-policy helper when the
			// event carries one (v1: EVENT_ACTION with a resolved role). Otherwise left None.
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

	std::int32_t SceneEventRelay::Register(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, std::string_view a_fn,
		std::int32_t a_sceneFilter, std::int32_t a_eventMask)
	{
		if (!a_receiver.get() || a_fn.empty()) {
			REX::WARN("SceneEventRelay::Register: null receiver or empty function name");
			return 0;
		}

		std::lock_guard l{ _lock };

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
				REX::ERROR("SceneEventRelay::Register: callback table full");
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
		e.fn = RE::BSFixedString(std::string(a_fn).c_str());
		e.sceneFilter = a_sceneFilter;
		e.eventMask = (a_eventMask == 0) ? Event::kAll : a_eventMask;

		const std::int32_t token = MakeToken(gen, slot);
		REX::INFO("SceneEventRelay: registered token {:#010x} -> {}(OSFEvent:SceneEvent) (mask {:#x}, scene {})",
			token, e.fn.c_str(), e.eventMask, e.sceneFilter);
		return token;
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
		REX::INFO("SceneEventRelay: unregistered token {:#010x}", a_token);
		return true;
	}

	void SceneEventRelay::Dispatch(const SceneEvent& a_event)
	{
		// Snapshot matching receivers under the lock; dispatch outside it (the VM call
		// may re-enter us via a callback that (un)registers).
		std::vector<std::pair<RE::BSTSmartPointer<RE::BSScript::Object>, RE::BSFixedString>> targets;
		{
			std::lock_guard l{ _lock };
			for (const auto& e : _slots) {
				if (e.generation == 0 || !e.receiver) {
					continue;
				}
				if ((e.eventMask & a_event.event) == 0) {
					continue;
				}
				if (e.sceneFilter != 0 && e.sceneFilter != a_event.scene) {
					continue;
				}
				targets.emplace_back(e.receiver, e.fn);
			}
		}
		if (targets.empty()) {
			return;
		}

		auto* vm = VM::GetSingleton();
		if (!vm) {
			REX::WARN("SceneEventRelay::Dispatch: no VM");
			return;
		}
		RE::BSScript::Variable payload;
		if (!PackPayload(vm, a_event, payload)) {
			return;
		}

		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
		for (auto& [receiver, fn] : targets) {
			vm->DispatchMethodCall(receiver, fn, MakeArgs(payload), noCallback, 0);
		}
	}

	bool SceneEventRelay::DispatchStatic(std::string_view a_script, std::string_view a_fn, const SceneEvent& a_event)
	{
		auto* vm = VM::GetSingleton();
		if (!vm) {
			REX::WARN("SceneEventRelay::DispatchStatic: no VM");
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

	void SceneEventRelay::Clear()
	{
		std::lock_guard l{ _lock };
		_slots.clear();
		_nextGen = 1;
	}
}
