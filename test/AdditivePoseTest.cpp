#include "Check.h"
#include "RotationSnapFixture.h"

#include "Animation/BlendSource.h"
#include "Animation/BoneMask.h"
#include "Animation/LiveBasePose.h"
#include "Animation/PoseMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using OSF::Test::Check;
using OSF::Test::Finish;

namespace
{
	using Matrix = std::array<float, 16>;
	using OSF::Animation::PoseMath::Quat;

	bool Near(float a_lhs, float a_rhs, float a_epsilon = 1e-4f)
	{
		return std::abs(a_lhs - a_rhs) <= a_epsilon;
	}

	Quat AxisAngle(float a_x, float a_y, float a_z, float a_radians)
	{
		const float half = a_radians * 0.5f;
		const float s = std::sin(half);
		return { std::cos(half), a_x * s, a_y * s, a_z * s };
	}

	Matrix Transform(const Quat& a_rotation = {}, float a_x = 0.0f, float a_y = 0.0f,
		float a_z = 0.0f, float a_scale = 1.0f)
	{
		Matrix out{};
		OSF::Animation::PoseMath::QuatToMatrix3x3(a_rotation, out.data());
		out[12] = a_x;
		out[13] = a_y;
		out[14] = a_z;
		out[15] = a_scale;
		return out;
	}

	void CheckZRotation(const Matrix& a_matrix, float a_radians, const char* a_message)
	{
		const float c = std::cos(a_radians);
		const float s = std::sin(a_radians);
		Check(Near(a_matrix[0], c) && Near(a_matrix[1], s) && Near(a_matrix[4], -s) && Near(a_matrix[5], c),
			a_message);
	}
}

int main()
{
	using namespace OSF::Animation::PoseMath;
	constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;

	// Captured player input changes smoothly while the shortest-arc override jumps
	// 76 degrees for one sample. Replaying those exact matrices must stay continuous.
	{
		OverrideBlendState continuity;
		Matrix previous{}, previousLegacy{};
		bool first = true;
		float legacyJump = 0.0f;
		const auto separation = [](const Matrix& a, const Matrix& b) {
			const auto qa = MatrixToUnitQuat(a.data()), qb = MatrixToUnitQuat(b.data());
			return 2.0f * std::acos(std::clamp(std::abs(qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z), 0.0f, 1.0f));
		};
		for (const auto& frame : RotationSnapFixture::frames) {
			Matrix out{}, legacy{};
			WriteOverrideTransition(legacy.data(), frame.live.data(), frame.sampled.data(), nullptr, 1.0f, 0.25f);
			Check(WriteOverrideTransition(out.data(), frame.live.data(), frame.sampled.data(), nullptr, 1.0f, 0.25f, &continuity),
				"captured spine override is written");
			if (!first) {
				legacyJump = std::max(legacyJump, separation(previousLegacy, legacy));
				Check(separation(previous, out) < 0.06f, "captured spine moves less than 3.5 degrees through the branch crossing");
			}
			Check(out[12] == legacy[12] && out[13] == legacy[13] && out[14] == legacy[14],
				"continuity changes no translation");
			previous = out; previousLegacy = legacy; first = false;
		}
		Check(legacyJump > 1.3f, "captured fixture reproduces the original greater-than-74-degree jump");
	}
	{
		for (const float strength : { 0.25f, 0.5f, 0.75f }) {
			OverrideBlendState continuity;
			Matrix previous{};
			bool first = true;
			for (const float degrees : { 178.0f, 179.0f, 180.0f, 181.0f, 182.0f, 181.0f, 180.0f, 179.0f, 178.0f }) {
				const Matrix live = Transform();
				const Matrix sampled = Transform(AxisAngle(0, 0, 1, degrees * std::numbers::pi_v<float> / 180.0f));
				Matrix out{};
				WriteOverrideTransition(out.data(), live.data(), sampled.data(), nullptr, 1.0f, strength, &continuity);
				if (!first) {
					Check(std::abs(out[0]-previous[0]) < 0.04f && std::abs(out[1]-previous[1]) < 0.04f,
						"feathered override remains continuous across 180 degrees in both directions");
				}
				previous = out; first = false;
			}
		}
		RotationBlendState state;
		state.valid = true;
		state.from = {};
		state.target = { -1.0f, 0.0f, 0.0f, 0.0f };
		const Quat same = NlerpContinuous({}, {}, 0.5f, state);
		Check(Near(same.w, 1.0f) && Near(same.x, 0.0f), "degenerate alternate arc cannot replace an identical endpoint");
	}

	{
		OSF::Animation::BlendSource::Prepared<Matrix> prepared;
		const std::vector<Matrix> sourcePose{
			Transform({}, 1.0f), Transform({}, 2.0f), Transform({}, 3.0f), Transform({}, 4.0f)
		};
		std::vector<std::uint8_t> driven{ 0, 1, 0, 1 };
		Check(prepared.Capture(sourcePose, driven), "replacement captures its visible blend source");
		driven.clear();  // graph teardown cannot invalidate the copied display ownership

		std::vector<Matrix> capturedPose;
		std::vector<std::uint8_t> capturedDriven;
		Check(prepared.Consume(sourcePose.size(), capturedPose, capturedDriven),
			"replacement consumes the source after binding teardown");
		Check(capturedPose == sourcePose, "replacement preserves the displayed outgoing pose");
		Check(capturedDriven == std::vector<std::uint8_t>{ 0, 1, 0, 1 },
			"replacement preserves exactly the joints driven by the outgoing graph");
		Check(!prepared.Consume(sourcePose.size(), capturedPose, capturedDriven),
			"prepared replacement source is one-shot");

		Check(prepared.Capture(sourcePose, std::vector<std::uint8_t>{ 0, 1, 0, 0 }),
			"replacement source can be prepared again");
		Check(!prepared.Consume(sourcePose.size() - 1, capturedPose, capturedDriven),
			"replacement rejects a source from a different skeleton");
		Check(!prepared.Consume(sourcePose.size(), capturedPose, capturedDriven),
			"a rejected replacement source is discarded");
		Check(!prepared.Capture(sourcePose, std::vector<std::uint8_t>{ 0, 0, 0, 0 }),
			"an entirely engine-driven display cannot become an outgoing layer");
		Check(!prepared.Capture(sourcePose, std::vector<std::uint8_t>{ 1 }),
			"capture rejects a mismatched display ownership table");
	}

	{
		Matrix live = Transform(AxisAngle(1.0f, 0.0f, 0.0f, 0.63f), 3.0f, -2.0f, 7.0f, 1.75f);
		live[3] = 11.0f;
		live[7] = 12.0f;
		live[11] = 13.0f;
		const Matrix reference = Transform(AxisAngle(0.0f, 1.0f, 0.0f, 0.42f), 9.0f, 8.0f, 7.0f, 0.5f);
		Matrix out{};
		WriteAdditive(out.data(), live.data(), reference.data(), reference.data(), 1.0f);
		Check(std::memcmp(out.data(), live.data(), sizeof(Matrix)) == 0,
			"reference sample leaves the live pose byte-for-byte unchanged");
	}

	{
		const Matrix live = Transform({}, 10.0f, 20.0f, 30.0f, 2.25f);
		const Matrix reference = Transform({}, 4.0f, 5.0f, 6.0f);
		const Matrix sampled = Transform({}, 8.0f, 3.0f, 10.0f);
		for (const auto [weight, multiplier] : std::array{
				std::pair{ 0.0f, 0.0f }, std::pair{ 0.5f, 0.5f }, std::pair{ 1.0f, 1.0f } }) {
			Matrix out{};
			WriteAdditive(out.data(), live.data(), reference.data(), sampled.data(), weight);
			Check(Near(out[12], 10.0f + 4.0f * multiplier) &&
				Near(out[13], 20.0f - 2.0f * multiplier) &&
				Near(out[14], 30.0f + 4.0f * multiplier),
				"translation delta respects weight 0/0.5/1");
			Check(out[15] == live[15], "additive translation keeps engine scale");
		}
	}

	{
		const Matrix live = Transform();
		const Matrix reference = Transform();
		const Matrix sampled = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi));
		for (const auto [weight, angle] : std::array{
				std::pair{ 0.0f, 0.0f }, std::pair{ 0.5f, kHalfPi * 0.5f }, std::pair{ 1.0f, kHalfPi } }) {
			Matrix out{};
			WriteAdditive(out.data(), live.data(), reference.data(), sampled.data(), weight);
			CheckZRotation(out, angle, "rotation delta respects weight 0/0.5/1");
		}
	}

	// final = live(X90) * delta(Z90): +X -> +Y under Z, then -> +Z under X.
	{
		const Matrix live = Transform(AxisAngle(1.0f, 0.0f, 0.0f, kHalfPi));
		const Matrix reference = Transform();
		const Matrix sampled = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi));
		Matrix out{};
		WriteAdditive(out.data(), live.data(), reference.data(), sampled.data(), 1.0f);
		Check(Near(out[0], 0.0f) && Near(out[1], 0.0f) && Near(out[2], 1.0f),
			"rotation composition order is live * reference-relative delta");
	}

	{
		std::array<Matrix, 2> slots{ Transform({}, 1.0f), Transform({}, 99.0f, 98.0f, 97.0f, 3.0f) };
		const Matrix preservedBefore = slots[1];
		const Matrix reference = Transform();
		const Matrix sampled = Transform({}, 5.0f);
		WriteAdditive(slots[0].data(), slots[0].data(), reference.data(), sampled.data(), 1.0f);
		Check(std::memcmp(slots[1].data(), preservedBefore.data(), sizeof(Matrix)) == 0,
			"a preserved/unbound bone stays byte-for-byte untouched");
	}

	{
		const Matrix from = Transform({}, 2.0f, 4.0f, 6.0f);
		const Matrix target = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi), 10.0f, 12.0f, 14.0f);
		Matrix endpoint{};
		WriteOverrideBlended(endpoint.data(), from.data(), target.data(), 0.0f);
		CheckZRotation(endpoint, 0.0f, "override blend at zero keeps the outgoing rotation");
		Check(Near(endpoint[12], 2.0f) && Near(endpoint[13], 4.0f) && Near(endpoint[14], 6.0f),
			"override blend at zero keeps the outgoing translation");

		Matrix out{};
		WriteOverrideBlended(out.data(), from.data(), target.data(), 0.5f);
		CheckZRotation(out, kHalfPi * 0.5f, "override keeps its existing normalized-lerp rotation");
		Check(Near(out[12], 6.0f) && Near(out[13], 8.0f) && Near(out[14], 10.0f) && out[15] == 1.0f,
			"override keeps its existing linear translation and unit scale");
	}

	// A feathered outgoing display is not the raw clip. Hold it through compose-before-evaluation,
	// crossfade to the fully composed incoming layer, and release outgoing-only bones continuously.
	{
		const Matrix live = Transform({}, 2.0f);
		const Matrix sampled = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi), 10.0f);
		Matrix outgoing{};
		WriteOverrideTransition(outgoing.data(), live.data(), sampled.data(), nullptr, 1.0f, 0.25f);
		Check(Near(outgoing[12], 4.0f), "outgoing snapshot includes the feathered layer strength");
		for (float strength : { 0.0f, 0.25f, 1.0f }) {
			Matrix held = live;
			Check(WriteOverrideTransition(held.data(), nullptr, sampled.data(), outgoing.data(), 0.0f, strength),
				"first compose stamps the outgoing display even without a new engine base");
			Check(held == outgoing, "zero-clock handoff preserves the displayed pose byte for byte");
		}
		Matrix waiting{};
		WriteOverrideTransition(waiting.data(), nullptr, sampled.data(), outgoing.data(), 0.5f, 0.25f);
		Check(waiting == outgoing, "a partial layer waits for an evaluated base without flashing vanilla");

		const Matrix movingLive = Transform({}, 6.0f);
		Matrix halfway{}, endpoint{}, steady{};
		WriteOverrideTransition(halfway.data(), movingLive.data(), sampled.data(), outgoing.data(), 0.5f, 0.25f);
		WriteOverrideTransition(endpoint.data(), movingLive.data(), sampled.data(), outgoing.data(), 1.0f, 0.25f);
		WriteOverrideTransition(steady.data(), movingLive.data(), sampled.data(), nullptr, 1.0f, 0.25f);
		Check(Near(halfway[12], 5.5f) && Near(endpoint[12], 7.0f),
			"transition interpolates to the incoming layer rather than applying its mask twice");
		Check(endpoint == steady, "dropping the transition snapshot at completion cannot change the pose");

		Matrix releasedHalf{}, releasedEnd{};
		WriteOverrideTransition(releasedHalf.data(), movingLive.data(), sampled.data(), outgoing.data(), 0.5f, 0.0f);
		WriteOverrideTransition(releasedEnd.data(), movingLive.data(), sampled.data(), outgoing.data(), 1.0f, 0.0f);
		Check(Near(releasedHalf[12], 5.0f) && releasedEnd == movingLive,
			"upper-body bones omitted by a right-arm mask fade to the moving live pose");

		OSF::Animation::BlendSource::Prepared<Matrix> interrupted;
		Check(interrupted.Capture(std::vector<Matrix>{ halfway }, std::vector<std::uint8_t>{ 1 }),
			"an interrupted blend captures its weighted display");
		std::vector<Matrix> source;
		std::vector<std::uint8_t> driven;
		Check(interrupted.Consume(1, source, driven), "interrupted display survives replacement teardown");
		Matrix reversed{};
		WriteOverrideTransition(reversed.data(), nullptr, live.data(), source[0].data(), 0.0f, 1.0f);
		Check(reversed == halfway, "reversal begins at the displayed blend instead of the raw outgoing clip");

		Matrix untouched = movingLive;
		Check(!WriteOverrideTransition(untouched.data(), nullptr, sampled.data(), nullptr, 0.5f, 0.25f) && untouched == movingLive,
			"newly included bones wait for a proven engine base");
		Check(!WriteOverrideTransition(untouched.data(), movingLive.data(), sampled.data(), nullptr, 0.0f, 1.0f),
			"initial zero-weight playback leaves vanilla alone without a prior owned pose");
		Check(!WriteOverrideTransition(untouched.data(), movingLive.data(), sampled.data(), nullptr, 1.0f, 0.0f),
			"zero-strength layers do not acquire ownership");
		Matrix repeated{};
		WriteOverrideTransition(repeated.data(), movingLive.data(), sampled.data(), outgoing.data(), 0.5f, 0.25f);
		Check(repeated == halfway, "repeated compose uses the same immutable base and display source");
	}

	{
		Check(Near(EffectiveWeight(0.5f, 0.25f), 0.125f),
			"blend transition and poseWeight multiply");
		const Matrix live = Transform();
		const Matrix reference = Transform();
		const Matrix sampled = Transform({}, 8.0f);
		Matrix blendIn{};
		Matrix blendOut{};
		WriteAdditive(blendIn.data(), live.data(), reference.data(), sampled.data(), EffectiveWeight(0.25f, 0.5f));
		WriteAdditive(blendOut.data(), live.data(), reference.data(), sampled.data(), EffectiveWeight(0.75f, 0.5f));
		Check(Near(blendIn[12], 1.0f) && Near(blendOut[12], 3.0f),
			"blend-in/out weights scale the persistent additive layer");
	}

	// Different references with the same authored delta must yield the same layer.
	{
		const Matrix live = Transform({}, 20.0f);
		const Matrix referenceA = Transform(AxisAngle(0.0f, 0.0f, 1.0f, 0.2f), 10.0f);
		const Matrix sampledA = Transform(AxisAngle(0.0f, 0.0f, 1.0f, 0.5f), 12.0f);
		const Matrix referenceB = Transform(AxisAngle(0.0f, 0.0f, 1.0f, 0.9f), 100.0f);
		const Matrix sampledB = Transform(AxisAngle(0.0f, 0.0f, 1.0f, 1.2f), 102.0f);
		Matrix stageA{};
		Matrix stageB{};
		WriteAdditive(stageA.data(), live.data(), referenceA.data(), sampledA.data(), 1.0f);
		WriteAdditive(stageB.data(), live.data(), referenceB.data(), sampledB.data(), 1.0f);
		Check(Near(stageA[12], 22.0f) && Near(stageB[12], 22.0f),
			"stage change uses its new translation reference");
		for (const int index : { 0, 1, 4, 5 }) {
			Check(Near(stageA[index], stageB[index]), "stage change uses its new rotation reference");
		}
	}

	{
		const Matrix immutableLive = Transform({}, 3.0f);
		const Matrix reference = Transform();
		const Matrix sampled = Transform({}, 2.0f);
		Matrix first{};
		Matrix repeated{};
		WriteAdditive(first.data(), immutableLive.data(), reference.data(), sampled.data(), 1.0f);
		WriteAdditive(repeated.data(), immutableLive.data(), reference.data(), sampled.data(), 1.0f);
		Check(Near(first[12], 5.0f) && Near(repeated[12], 5.0f),
			"repeated stamping does not accumulate an already-written delta");
	}

	// A 3D rebuild can reorder rig slots; the carried base must follow joint identity.
	{
		struct Binding
		{
			std::uint16_t rigIndex;
			std::uint16_t jointIndex;
		};
		const std::array oldBinding{ Binding{ 8, 2 }, Binding{ 3, 5 } };
		const auto joint2 = Transform({}, 2.0f);
		const auto joint5 = Transform({}, 5.0f);
		std::array<float, 32> oldBase{};
		std::copy(joint2.begin(), joint2.end(), oldBase.begin());
		std::copy(joint5.begin(), joint5.end(), oldBase.begin() + 16);
		OSF::Animation::LiveBasePose::Cache cache;
		cache.Store(std::span{ oldBinding }, std::span<const float>{ oldBase }, 8);

		const std::array rebound{ Binding{ 11, 5 }, Binding{ 1, 2 } };
		std::array<float, 32> restored{};
		const bool complete = cache.Restore(std::span{ rebound }, std::span<float>{ restored });
		Check(complete && std::memcmp(restored.data(), joint5.data(), sizeof(Matrix)) == 0 &&
			std::memcmp(restored.data() + 16, joint2.data(), sizeof(Matrix)) == 0,
			"rig rebind restores the live base by joint identity, not old slot order");

		const std::array missing{ Binding{ 4, 7 } };
		std::array<float, 16> missingPose{};
		Check(!cache.Restore(std::span{ missing }, std::span<float>{ missingPose }),
			"rig rebind refuses an incomplete carried base");
	}

	// A compose-time rebind must invalidate even before the engine revision counter advances.
	{
		OSF::Animation::LiveBasePose::Evaluation evaluated;
		evaluated.Mark(12, 4);
		Check(evaluated.IsCurrent(12, 4), "current binding accepts its post-engine evaluation");
		Check(!evaluated.IsCurrent(12, 5),
			"compose-time rebind cannot capture under the previous binding authorization");
		evaluated.Mark(13, 5);
		Check(evaluated.IsCurrent(13, 5),
			"new binding becomes capture-ready after its own post-engine evaluation");
	}

	{
		namespace BoneMask = OSF::Animation::BoneMask;
		const auto* upper = BoneMask::Find("upperBody");
		Check(upper != nullptr && upper == BoneMask::Find("UPPERBODY") && upper->id == "upperBody",
			"mask lookup is case-insensitive and returns the canonical id");
		Check(BoneMask::Find("torso") == nullptr, "an unknown mask name resolves to null");
		Check(upper && upper->feathered &&
			Near(upper->weights.at("c_spine"), 0.25f) &&
			Near(upper->weights.at("c_spine1"), 0.5f) &&
			Near(upper->weights.at("c_spine2"), 0.75f) &&
			Near(upper->weights.at("c_chest"), 1.0f),
			"upperBody feathers the spine seam up to a full-weight chest");
		Check(upper && upper->weights.contains("l_wrist") && upper->weights.contains("r_clavicle") &&
			upper->weights.contains("c_head") && upper->weights.contains("weapon") &&
			!upper->weights.contains("c_hips") && !upper->weights.contains("l_thigh") &&
			!upper->weights.contains("directat"),
			"upperBody covers both arm chains + head/weapon and excludes the lower body");
		const auto* arms = BoneMask::Find("arms");
		Check(arms && !arms->feathered && arms->weights.contains("l_clavicle") &&
			arms->weights.contains("r_index2") && !arms->weights.contains("c_spine"),
			"arms is a hard clavicle-down mask with no spine seam");
		const auto* leftArm = BoneMask::Find("leftArm");
		Check(leftArm && leftArm->weights.contains("l_biceps") && leftArm->weights.contains("weaponleft") &&
			!leftArm->weights.contains("r_biceps") && !leftArm->weights.contains("weapon"),
			"leftArm stays one-sided");
		const auto* lower = BoneMask::Find("lowerBody");
		Check(lower && !lower->feathered && lower->weights.contains("c_hips") &&
			lower->weights.contains("r_toe") && !lower->weights.contains("com") &&
			!lower->weights.contains("l_clavicle"),
			"lowerBody covers hips-down and leaves COM/root motion engine-driven");
		for (const auto& mask : BoneMask::All()) {
			bool weightsValid = true;
			for (const auto& [bone, weight] : mask.weights) {
				weightsValid = weightsValid && weight > 0.0f && weight <= 1.0f &&
					bone == BoneMask::detail::Lower(bone);
			}
			Check(weightsValid, "every mask entry is lowercased with a weight in (0, 1]");
		}
		Check(BoneMask::KnownList().find("upperBody") != std::string::npos,
			"the diagnostic known-mask list names the canonical ids");
	}

	return Finish("Additive pose");
}
