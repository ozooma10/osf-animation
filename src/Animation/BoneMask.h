#pragma once

// Partial-body masks: weight-1 bones take the sampled pose, fractional bones blend sampled-vs-live
// (feathered seam); unlisted bones stay engine-driven. Names match rig nodes case-insensitively.
// Engine-free: the standalone pose tests compile this header.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSF::Animation::BoneMask
{
	struct Mask
	{
		std::string id;  // lookups are case-insensitive
		// lowercased rig bone name -> weight in (0, 1]
		std::unordered_map<std::string, float> weights;
		bool feathered = false;  // any weight < 1 (needs the live-base blend path)
	};

	namespace detail
	{
		inline std::string Lower(std::string_view a_text)
		{
			std::string out{ a_text };
			for (auto& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		inline void Add(Mask& a_mask, std::string_view a_bone, float a_weight = 1.0f)
		{
			a_mask.weights.emplace(Lower(a_bone), a_weight);
			if (a_weight < 1.0f) {
				a_mask.feathered = true;
			}
		}

		inline void AddHand(Mask& a_mask, std::string_view a_side)
		{
			static constexpr std::string_view kHand[] = {
				"wrist", "thumb", "thumb1", "thumb2", "cup",
				"pinky", "pinky1", "pinky2", "ring", "ring1", "ring2",
				"middle", "middle1", "middle2", "index", "index1", "index2",
				"animobject1", "animobject2", "animobject3"
			};
			for (const auto part : kHand) {
				Add(a_mask, std::string(a_side) + "_" + std::string(part));
			}
		}

		inline void AddArm(Mask& a_mask, std::string_view a_side)
		{
			static constexpr std::string_view kArm[] = {
				"clavicle", "biceps", "biceps_twist", "biceps_twist1",
				"forearm", "arm", "wrist_twist", "wrist_twist1", "wrist_twist2"
			};
			for (const auto part : kArm) {
				Add(a_mask, std::string(a_side) + "_" + std::string(part));
			}
			AddHand(a_mask, a_side);
		}

		inline void AddLeg(Mask& a_mask, std::string_view a_side)
		{
			static constexpr std::string_view kLeg[] = {
				"thigh", "thigh_twist", "thigh_twist1", "calf", "foot", "toe"
			};
			for (const auto part : kLeg) {
				Add(a_mask, std::string(a_side) + "_" + std::string(part));
			}
		}

		inline std::vector<Mask> Build()
		{
			std::vector<Mask> masks;

			// upperBody omits DirectAt/Eye_Target so engine look-at keeps working.
			{
				Mask& m = masks.emplace_back();
				m.id = "upperBody";
				Add(m, "c_spine", 0.25f);
				Add(m, "c_spine1", 0.5f);
				Add(m, "c_spine2", 0.75f);
				Add(m, "c_chest");
				Add(m, "c_neck");
				Add(m, "c_neck1");
				Add(m, "c_neck_twist");
				Add(m, "c_head");
				Add(m, "weapon");
				Add(m, "weaponleft");
				Add(m, "l_handik");
				Add(m, "r_handik");
				AddArm(m, "l");
				AddArm(m, "r");
			}
			{
				Mask& m = masks.emplace_back();
				m.id = "arms";
				Add(m, "weapon");
				Add(m, "weaponleft");
				Add(m, "l_handik");
				Add(m, "r_handik");
				AddArm(m, "l");
				AddArm(m, "r");
			}
			{
				Mask& m = masks.emplace_back();
				m.id = "leftArm";
				Add(m, "weaponleft");
				Add(m, "l_handik");
				AddArm(m, "l");
			}
			{
				Mask& m = masks.emplace_back();
				m.id = "rightArm";
				Add(m, "weapon");
				Add(m, "r_handik");
				AddArm(m, "r");
			}
			{
				Mask& m = masks.emplace_back();
				m.id = "lowerBody";
				Add(m, "c_hips");
				Add(m, "c_waist");
				AddLeg(m, "l");
				AddLeg(m, "r");
			}
			{
				Mask& m = masks.emplace_back();
				m.id = "hands";
				AddHand(m, "l");
				AddHand(m, "r");
			}
			return masks;
		}
	}

	inline const std::vector<Mask>& All()
	{
		static const std::vector<Mask> masks = detail::Build();
		return masks;
	}

	// nullptr if unknown; the result has static storage.
	inline const Mask* Find(std::string_view a_name)
	{
		const auto lower = detail::Lower(a_name);
		for (const auto& mask : All()) {
			if (detail::Lower(mask.id) == lower) {
				return &mask;
			}
		}
		return nullptr;
	}

	inline std::string KnownList()
	{
		std::string out;
		for (const auto& mask : All()) {
			if (!out.empty()) {
				out += ", ";
			}
			out += mask.id;
		}
		return out;
	}
}
