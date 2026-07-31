#include "Props/PropService.h"

#include "Util/FormRef.h"
#include "Util/SceneGraph.h"

#include "RE/T/TESObjectARMO.h"

#include <cmath>
#include <utility>

namespace OSF::Props
{
	namespace
	{
		constexpr float kDegreesToRadians = 0.01745329251994329577f;

		void Fail(std::string* a_error, std::string a_message)
		{
			if (a_error) {
				*a_error = std::move(a_message);
			}
		}

		::RE::NiPointer<::RE::NiAVObject> ResolveActorRoot(::RE::Actor* a_actor)
		{
			::RE::NiPointer<::RE::NiAVObject> root;
			if (a_actor) {
				const auto loaded = a_actor->loadedData.LockRead();
				if (*loaded) {
					root = (*loaded)->data3D;
				}
			}
			return root;
		}

		::RE::TESBoundObject* ResolveEquippedArmor(
			::RE::Actor* a_actor, const std::vector<std::string>& a_keywords)
		{
			if (!a_actor || a_keywords.empty()) {
				return nullptr;
			}

			std::vector<::RE::BGSKeyword*> keywords;
			keywords.reserve(a_keywords.size());
			for (const auto& editorID : a_keywords) {
				auto* keyword = ::RE::TESForm::LookupByEditorID<::RE::BGSKeyword>(
					::RE::BSFixedString(editorID.c_str()));
				if (keyword) {
					keywords.push_back(keyword);
				} else {
					// The list is any-of: a missing fallback is expected when another
					// editor ID resolves on this load order. The enclosing prop action
					// emits the actionable warning if none of the alternatives works.
					REX::DEBUG("[Scene] equipped-armor prop source alternative keyword '{}' was not found", editorID);
				}
			}
			if (keywords.empty()) {
				return nullptr;
			}

			::RE::TESBoundObject* result = nullptr;
			a_actor->ForEachEquippedItem([&](const ::RE::BGSInventoryItem& a_item) {
				auto* armor = a_item.object ?
					a_item.object->As<::RE::TESObjectARMO>() : nullptr;
				if (!armor) {
					return ::RE::BSContainer::ForEachResult::kContinue;
				}
				for (auto* keyword : keywords) {
					if (armor->HasKeyword(keyword)) {
						result = armor;
						return ::RE::BSContainer::ForEachResult::kStop;
					}
				}
				return ::RE::BSContainer::ForEachResult::kContinue;
			});
			return result;
		}

		::RE::TESBoundObject* ResolveSource(
			::RE::Actor* a_actor, const Source& a_source)
		{
			switch (a_source.kind) {
			case SourceKind::kForm:
				return Util::ResolveBoundObject(a_source.form);
			case SourceKind::kEquippedArmor:
				return ResolveEquippedArmor(a_actor, a_source.keywords);
			default:
				return nullptr;
			}
		}

		// Authored XYZ degrees, in the authoring tool's sense of them.
		//
		// The body builds the textbook right-handed Rz*Ry*Rx for column vectors,
		// but the engine reads local.rotate in the transposed (frame-orientation)
		// convention and therefore renders that matrix's INVERSE. Proven in game
		// 2026-07-30 on the Suit Protocol helmet proxy, which shares this exact
		// builder: identity matched the authoring tool while Rx/Ry/Rz(+90) each
		// rendered as their negative, and all three matched after transposing.
		// For a pure rotation the inverse IS the transpose, so transposing at
		// this authored-euler boundary makes an authored angle mean the same
		// thing in Blender, Studio, and the running game. Do not "fix" a
		// mirrored angle by flipping its sign in a scene file - the correction
		// belongs here, once.
		::RE::NiMatrix3 RotationFromDegrees(
			const std::array<float, 3>& a_rotation)
		{
			const float x = a_rotation[0] * kDegreesToRadians;
			const float y = a_rotation[1] * kDegreesToRadians;
			const float z = a_rotation[2] * kDegreesToRadians;
			const float sx = std::sin(x);
			const float cx = std::cos(x);
			const float sy = std::sin(y);
			const float cy = std::cos(y);
			const float sz = std::sin(z);
			const float cz = std::cos(z);

			const ::RE::NiMatrix3 rotateX{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, cx, -sx, 0.0f,
				0.0f, sx, cx, 0.0f
			};
			const ::RE::NiMatrix3 rotateY{
				cy, 0.0f, sy, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				-sy, 0.0f, cy, 0.0f
			};
			const ::RE::NiMatrix3 rotateZ{
				cz, -sz, 0.0f, 0.0f,
				sz, cz, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f
			};
			return (rotateZ * rotateY * rotateX).Transpose();
		}

		void ApplyTransform(
			::RE::NiAVObject& a_object, const Attachment& a_attachment)
		{
			a_object.local.translate = {
				a_attachment.position[0],
				a_attachment.position[1],
				a_attachment.position[2]
			};
			a_object.local.rotate = RotationFromDegrees(a_attachment.rotation);
			a_object.local.scale = a_attachment.scale;
		}

		::RE::NiNode* ResolveAttachmentNode(
			::RE::NiAVObject* a_root, std::string_view a_name)
		{
			if (!a_root) {
				return nullptr;
			}
			if (a_name == "$ActorRoot") {
				return a_root->GetAsNiNode();
			}
			const std::string name{ a_name };
			const ::RE::BSFixedString nodeName{ name.c_str() };
			auto* target = a_root->GetObjectByName(nodeName);
			return target ? target->GetAsNiNode() : nullptr;
		}
	}

	PropService& PropService::GetSingleton()
	{
		static PropService instance;
		return instance;
	}

	bool PropService::Available()
	{
		return OSF::RE::EnsureResolved();
	}

	Instance PropService::CreateAttached(
		::RE::Actor* a_actor, const Source& a_source,
		const Attachment& a_attachment, std::string* a_error)
	{
		Instance result;
		if (!a_actor) {
			Fail(a_error, "role resolved no actor");
			return result;
		}
		if (!Available()) {
			Fail(a_error, OSF::RE::StatusText());
			return result;
		}

		auto* source = ResolveSource(a_actor, a_source);
		if (!source) {
			Fail(a_error, "prop source resolved no equipped or loaded form");
			return result;
		}
		if (!OSF::RE::CreateWorldModelVisual(source, result.object, a_error)) {
			return {};
		}
		result.sourceForm = source->formID;
		result.object->name = "OSF_SceneProp";

		if (!Attach(result, a_actor, a_attachment, a_error)) {
			result.object.reset();
			result.actorRoot.reset();
			result.sourceForm = 0;
			return {};
		}
		return result;
	}

	bool PropService::Attach(
		Instance& a_instance, ::RE::Actor* a_actor,
		const Attachment& a_attachment,
		std::string* a_error)
	{
		if (!a_instance.object) {
			Fail(a_error, "scene prop instance is empty");
			return false;
		}
		auto actorRoot = ResolveActorRoot(a_actor);
		if (!actorRoot) {
			Fail(a_error, "actor 3D root is unavailable");
			return false;
		}
		auto* target = ResolveAttachmentNode(actorRoot.get(), a_attachment.node);
		if (!target) {
			Fail(a_error, "attachment node '" + a_attachment.node +
				"' was not found or is not an NiNode");
			return false;
		}

		auto* current = OSF::RE::GetParent(a_instance.object.get());
		ApplyTransform(*a_instance.object, a_attachment);
		if (current == target) {
			a_instance.actorRoot = std::move(actorRoot);
			return true;
		}

		const bool attached = current ?
			OSF::RE::Reparent(a_instance.object.get(), target, a_error) :
			OSF::RE::AttachChild(target, a_instance.object.get(), a_error);
		if (!attached || OSF::RE::GetParent(a_instance.object.get()) != target) {
			return false;
		}
		a_instance.actorRoot = std::move(actorRoot);
		return true;
	}

	bool PropService::Destroy(Instance& a_instance, std::string* a_error)
	{
		if (!a_instance.object) {
			a_instance.actorRoot.reset();
			a_instance.sourceForm = 0;
			return true;
		}

		if (auto* parent = OSF::RE::GetParent(a_instance.object.get())) {
			if (!OSF::RE::DetachChild(
					parent, a_instance.object.get(), a_error)) {
				return false;
			}
		}
		a_instance.object.reset();
		a_instance.actorRoot.reset();
		a_instance.sourceForm = 0;
		return true;
	}
}
