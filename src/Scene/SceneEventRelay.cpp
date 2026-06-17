#include "Scene/SceneEventRelay.h"

namespace OSF::Scene
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		// Build the frozen-layout Var[] payload (see SceneEvent / OSFEvent.psc). Returns
		// null if the VM can't allocate the array.
		RE::BSTSmartPointer<RE::BSScript::Array> PackPayload(VM* a_vm, const SceneEvent& a_event)
		{
			RE::BSTSmartPointer<RE::BSScript::Array> arr;
			// Element type kVar => a Papyrus Var[] (the container provides the array-ness).
			if (!a_vm->CreateArray(RE::BSScript::TypeInfo::RawType::kVar, RE::BSFixedString{}, 12u, arr) || !arr) {
				return nullptr;
			}

			auto str = [](const std::string& s) { return RE::BSFixedString(s.c_str()); };
			(*arr)[0] = a_event.scene;
			(*arr)[1] = a_event.event;
			(*arr)[2] = str(a_event.node);
			(*arr)[3] = str(a_event.edge);
			(*arr)[4] = str(a_event.cue);
			(*arr)[5] = str(a_event.actionType);
			// [6] actor: left as None for Phase A (NODE_ENTER/EXIT carry no actor). Real
			// object marshalling lands with the events that populate a role/actor.
			(*arr)[7] = str(a_event.role);
			(*arr)[8] = a_event.loopIndex;
			(*arr)[9] = a_event.time;
			(*arr)[10] = str(a_event.anchor);
			(*arr)[11] = a_event.result;
			return arr;
		}

		// A one-argument call: args[0] = the Var[] payload. Capturing the array keeps it
		// alive until the (async) Papyrus stack consumes it.
		auto MakeArgs(RE::BSTSmartPointer<RE::BSScript::Array> a_payload)
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
		REX::INFO("SceneEventRelay: registered token {:#010x} -> {}(Var[]) (mask {:#x}, scene {})",
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
		auto payload = PackPayload(vm, a_event);
		if (!payload) {
			REX::WARN("SceneEventRelay::Dispatch: failed to build payload array");
			return;
		}

		REX::INFO("SceneEventRelay::Dispatch: event={:#x} scene={} -> {} receiver(s)",
			a_event.event, a_event.scene, targets.size());
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
		auto payload = PackPayload(vm, a_event);
		if (!payload) {
			return false;
		}
		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
		const bool queued = vm->DispatchStaticCall(
			RE::BSFixedString(std::string(a_script).c_str()),
			RE::BSFixedString(std::string(a_fn).c_str()),
			MakeArgs(payload), noCallback, 0);
		REX::INFO("SceneEventRelay::DispatchStatic: {}.{}(Var[]) queued={}", a_script, a_fn, queued);
		return queued;
	}

	void SceneEventRelay::Clear()
	{
		std::lock_guard l{ _lock };
		_slots.clear();
		_nextGen = 1;
	}
}
