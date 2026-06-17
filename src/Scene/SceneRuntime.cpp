#include "Scene/SceneRuntime.h"

#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Util/StringUtil.h"

namespace OSF::Scene
{
	SceneRuntime& SceneRuntime::GetSingleton()
	{
		static SceneRuntime singleton;
		return singleton;
	}

	SceneRuntime::Slot* SceneRuntime::Resolve(std::int32_t a_handle)
	{
		if (a_handle == 0) {
			return nullptr;
		}
		const auto slot = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		const auto gen = static_cast<std::uint16_t>((a_handle >> 16) & 0xFFFF);
		if (slot >= _slots.size() || _slots[slot].generation != gen) {
			return nullptr;
		}
		return &_slots[slot];
	}

	void SceneRuntime::Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor)
	{
		// Logged so the lifecycle is visible even with no registered receiver; the relay
		// delivers the OSFEvent:SceneEvent struct to any that are registered.
		REX::INFO("SceneRuntime: scene {:#010x} event {:#x} node='{}' anchor='{}'", a_handle, a_event, a_node, a_anchor);
		SceneEvent e;
		e.scene = a_handle;
		e.event = a_event;
		e.node = std::string(a_node);
		e.anchor = std::string(a_anchor);
		// actor/role left default (Phase A).
		SceneEventRelay::GetSingleton().Dispatch(e);
	}

	std::int32_t SceneRuntime::Start(std::string_view a_id, std::string_view a_entryNode,
		const std::vector<RE::Actor*>& a_participants)
	{
		std::int32_t handle = 0;
		std::string node;
		{
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
					REX::ERROR("SceneRuntime::Start: handle table full");
					return 0;
				}
				_slots.emplace_back();
			}

			const std::uint16_t gen = _nextGen++;
			if (_nextGen == 0) {
				_nextGen = 1;  // never hand out generation 0 (the empty-slot marker)
			}

			Slot& s = _slots[slot];
			s.generation = gen;
			s.id = std::string(a_id);
			s.node = std::string(a_entryNode);
			s.stage = 0;
			s.participants = a_participants;

			handle = MakeToken(gen, slot);
			node = s.node;
		}

		Fire(handle, Event::kNodeEnter, node, "enter");
		return handle;
	}

	bool SceneRuntime::SetNode(std::int32_t a_scene, std::string_view a_node, std::int32_t a_stage)
	{
		std::string oldNode;
		std::string newNode;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			oldNode = s->node;
			s->node = std::string(a_node);
			s->stage = a_stage;
			newNode = s->node;
		}

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		Fire(a_scene, Event::kNodeEnter, newNode, "enter");
		return true;
	}

	bool SceneRuntime::Stop(std::int32_t a_scene)
	{
		std::string node;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			node = s->node;
			*s = Slot{};  // generation 0 → empty; invalidates the handle
		}

		Fire(a_scene, Event::kNodeExit, node, "exit");
		Fire(a_scene, Event::kSceneEnd, node, "");
		return true;
	}

	std::int32_t SceneRuntime::StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants)
	{
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::WARN("SceneRuntime::StartFromDef: no scene def '{}'", a_sceneId);
			return 0;
		}
		// Start mints the handle, records the instance, and fires NODE_ENTER for the entry.
		return Start(def->id, def->entry, a_participants);
	}

	bool SceneRuntime::Advance(std::int32_t a_scene)
	{
		std::string oldNode;
		std::string newNode;
		bool end = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (!node) {
				return false;  // not def-backed, or current node not in the def
			}
			const Registry::SceneEdge* edge = nullptr;
			for (const auto& e : node->edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && e.isDefault) {
					edge = &e;
					break;
				}
			}
			if (!edge) {
				return false;  // no default advance edge — never inferred
			}
			oldNode = s->node;
			if (edge->to == "$end") {
				end = true;
				*s = Slot{};  // free the handle
			} else {
				s->node = edge->to;
				newNode = edge->to;
			}
		}

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		Fire(a_scene, end ? Event::kSceneEnd : Event::kNodeEnter, end ? oldNode : newNode, end ? "" : "enter");
		return true;
	}

	bool SceneRuntime::Navigate(std::int32_t a_scene, std::string_view a_edgeId)
	{
		const auto wantId = Util::ToLower(std::string(a_edgeId));
		std::string oldNode;
		std::string newNode;
		bool end = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (!node) {
				return false;
			}
			const Registry::SceneEdge* edge = nullptr;
			for (const auto& e : node->edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && Util::ToLower(e.id) == wantId) {
					edge = &e;
					break;
				}
			}
			if (!edge) {
				return false;  // no such branchable edge on the current node
			}
			oldNode = s->node;
			if (edge->to == "$end") {
				end = true;
				*s = Slot{};
			} else {
				s->node = edge->to;
				newNode = edge->to;
			}
		}

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		Fire(a_scene, end ? Event::kSceneEnd : Event::kNodeEnter, end ? oldNode : newNode, end ? "" : "enter");
		return true;
	}

	std::int32_t SceneRuntime::EdgeCount(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return 0;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return 0;
		}
		std::int32_t count = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				count++;
			}
		}
		return count;
	}

	std::string SceneRuntime::EdgeId(std::int32_t a_scene, std::int32_t a_index)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return {};
		}
		std::int32_t i = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				if (i == a_index) {
					return e.id;
				}
				i++;
			}
		}
		return {};
	}

	std::string SceneRuntime::EdgeLabel(std::int32_t a_scene, std::int32_t a_index)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return {};
		}
		std::int32_t i = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				if (i == a_index) {
					// labelKey (a localization token) if present, else the literal label.
					return e.labelKey.empty() ? e.label : e.labelKey;
				}
				i++;
			}
		}
		return {};
	}

	std::string SceneRuntime::GetId(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s ? s->id : std::string{};
	}

	std::string SceneRuntime::GetNode(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s ? s->node : std::string{};
	}

	std::int32_t SceneRuntime::GetStage(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s ? s->stage : -1;
	}

	std::int32_t SceneRuntime::GetSceneForActor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0;
		}
		std::lock_guard l{ _lock };
		for (std::uint16_t slot = 0; slot < _slots.size(); slot++) {
			const Slot& s = _slots[slot];
			if (s.generation == 0) {
				continue;
			}
			for (auto* p : s.participants) {
				if (p == a_actor) {
					return MakeToken(s.generation, slot);
				}
			}
		}
		return 0;
	}

	void SceneRuntime::Clear()
	{
		std::lock_guard l{ _lock };
		_slots.clear();
		_nextGen = 1;
	}
}
