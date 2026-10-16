#include "Scene/SceneRuntime.h"
#include "Scene/AlignmentStore.h"
#include "Animation/GraphManager.h"
#include "UI/HudMessage.h"

namespace OSF::Scene
{
	namespace
	{
		std::vector<std::string> RoleNames(const Registry::SceneDef& def)
		{
			std::vector<std::string> names;
			for (const auto& role : def.roles) names.push_back(role.name);
			return names;
		}
	}
	bool SceneRuntime::SetAlignment(std::int32_t a_scene, std::string_view a_role, const Alignment::Delta& a_delta)
	{
		if (!Alignment::Valid(a_delta)) return false;
		std::lock_guard l{ _lock };
		auto* s = Resolve(a_scene);
		if (!s || !s->definition || s->transitioning) return false;
		const auto names = RoleNames(*s->definition);
		const auto role = std::ranges::find(names, a_role);
		if (role == names.end() || names.size() != s->participants.size()) return false;
		const auto index = static_cast<std::size_t>(role - names.begin());
		if (!Animation::GraphManager::GetSingleton().SetAlignmentDelta(s->participants[index], a_delta, s->playbackId)) return false;
		s->align.Enter(s->node, Alignment::Read(s->id, s->node, names))[index] = a_delta;
		return true;
	}
	std::optional<Alignment::Delta> SceneRuntime::GetAlignment(std::int32_t a_scene, std::string_view a_role)
	{
		std::lock_guard l{ _lock };
		auto* s = Resolve(a_scene);
		if (!s || !s->definition) return std::nullopt;
		const auto names = RoleNames(*s->definition);
		const auto role = std::ranges::find(names, a_role);
		if (role == names.end() || names.size() != s->participants.size()) return std::nullopt;
		return s->align.Enter(s->node, Alignment::Read(s->id, s->node, names))[role - names.begin()];
	}
	bool SceneRuntime::SaveAlignment(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		auto* s = Resolve(a_scene);
		if (!s || !s->definition || s->transitioning ||
			s->worldPlacement.value_or(s->definition->worldPlacement) == Animation::WorldPlacementMode::kFollowActor) return false;
		const auto names = RoleNames(*s->definition);
		return Alignment::Save(s->id, s->node, names, s->align.Enter(s->node, Alignment::Read(s->id, s->node, names)));
	}
	void SceneRuntime::AlignmentInput(std::int32_t a_scene, Input::Verb a_verb)
	{
		using Input::Verb;
		std::string role;
		Alignment::Delta value;
		bool fine;
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(a_scene);
			if (!s || s->transitioning || !s->definition || s->definition->roles.empty() ||
				s->worldPlacement.value_or(s->definition->worldPlacement) == Animation::WorldPlacementMode::kFollowActor) return;
			const auto names = RoleNames(*s->definition);
			if (a_verb == Verb::kAlignSelect) s->alignSelected = (s->alignSelected + 1) % names.size();
			if (a_verb == Verb::kAlignStep) s->alignFine = !s->alignFine;
			fine = s->alignFine;
			role = names[s->alignSelected];
			value = s->align.Enter(s->node, Alignment::Read(s->id, s->node, names))[s->alignSelected];
		}
		const float step = fine ? 0.1f : 1.0f;
		const float angle = (fine ? 1.0f : 5.0f) * Util::kDegToRadF;
		switch (a_verb) {
		case Verb::kAlignXPlus: value.x += step; break; case Verb::kAlignXMinus: value.x -= step; break;
		case Verb::kAlignYPlus: value.y += step; break; case Verb::kAlignYMinus: value.y -= step; break;
		case Verb::kAlignZPlus: value.z += step; break; case Verb::kAlignZMinus: value.z -= step; break;
		case Verb::kAlignHeadingPlus: value.heading += angle; break; case Verb::kAlignHeadingMinus: value.heading -= angle; break;
		case Verb::kAlignReset: value = {}; break;
		case Verb::kAlignSave:
			if (!SaveAlignment(a_scene)) { UI::HudMessage::Error("alignment could not be saved"); return; }
			break;
		default: break;
		}
		if (a_verb != Verb::kAlignSave && !SetAlignment(a_scene, role, value)) return;
		bool dirty = false;
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(a_scene);
			if (!s) return;
			const auto saved = Alignment::Read(s->id, s->node, RoleNames(*s->definition));
			const auto& working = s->align.nodes.at(s->node);
			dirty = !std::equal(working.begin(), working.end(), saved.begin(), saved.end(), Alignment::Equal);
		}
		UI::HudMessage::Show(std::format("OSF: {} X {:.1f} Y {:.1f} Z {:.1f} heading {:.1f} | {}{}",
			role, value.x, value.y, value.z, value.heading / Util::kDegToRadF, fine ? "fine" : "coarse", dirty ? " | unsaved" : ""));
	}
}
