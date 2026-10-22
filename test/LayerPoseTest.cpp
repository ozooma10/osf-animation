#include "Check.h"

#include "Animation/LayerCompose.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

using OSF::Test::Check;
using OSF::Test::Finish;

namespace
{
	using Matrix = std::array<float, 16>;
	using OSF::Animation::LayerCompose::Bone;
	using OSF::Animation::LayerCompose::Layer;
	using OSF::Animation::LayerCompose::Mode;
	using OSF::Animation::PoseMath::Quat;

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

	Layer MakeLayer(Mode a_mode, float a_transition, float a_poseWeight,
		const Matrix& a_sampled, const Bone& a_bone, const Matrix* a_displayed = nullptr,
		const std::uint8_t* a_displayedDriven = nullptr)
	{
		return { a_mode, a_poseWeight, a_transition, a_sampled.data(), nullptr,
			a_displayed ? a_displayed->data() : nullptr, a_displayedDriven, &a_bone, 1 };
	}

	Layer MakeAdditive(float a_weight, const Matrix& a_sampled, const Matrix& a_reference,
		const Bone& a_bone, float a_transition = 1.0f)
	{
		Layer layer = MakeLayer(Mode::kAdditive, a_transition, a_weight, a_sampled, a_bone);
		layer.reference = a_reference.data();
		return layer;
	}
}

int main()
{
	using OSF::Animation::LayerCompose::Compose;
	using namespace OSF::Animation::PoseMath;
	constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;

	// L7.1: override equality, including declined writes and per-joint snapshot ownership.
	{
		const Matrix base = Transform(AxisAngle(1, 0, 0, kHalfPi), 1.0f, 2.0f, 3.0f);
		const Matrix sampled = Transform(AxisAngle(0, 0, 1, kHalfPi), 4.0f, 5.0f, 6.0f);
		const Matrix displayed = Transform(AxisAngle(0, 1, 0, kHalfPi), 7.0f, 8.0f, 9.0f);
		struct Case
		{
			float transition;
			float poseWeight;
			float maskWeight;
			bool snapshot;
			bool haveBase;
			bool driven;
		};
		for (const Case c : {
				Case{ 1.0f, 1.0f, 1.0f, false, true, true },   // full override
				Case{ 1.0f, 1.0f, 0.5f, false, true, true },   // feathered bone
				Case{ 0.4f, 1.0f, 1.0f, true, true, true },    // mid-crossfade
				Case{ 1.0f, 1.0f, 0.5f, true, false, true },   // unproven base: hold snapshot
				Case{ 0.0f, 1.0f, 1.0f, false, true, false },  // declined: transition zero
				Case{ 1.0f, 0.0f, 1.0f, false, true, false },  // declined: strength zero
				Case{ 0.4f, 0.5f, 0.5f, true, true, true },    // mask does not weight crossfade
				Case{ 0.4f, 1.0f, 0.0f, true, true, true },    // outgoing bone still fading
				Case{ 1.0f, 1.0f, 0.0f, true, true, false },   // completed release
				Case{ 0.0f, 1.0f, 1.0f, true, false, true },   // first frame holds display
				Case{ 1.0f, 0.5f, 1.0f, false, false, false }, // declined: no base or snapshot
				Case{ 1.0f, 1.0f, 1.0f, false, false, true },  // absolute override needs no base
				Case{ 1.5f, 3.0f, 0.5f, false, true, true } }) { // helper clamps layer inputs
			Matrix expected = base;
			WriteOverrideTransition(expected.data(), c.haveBase ? base.data() : nullptr,
				sampled.data(), c.snapshot ? displayed.data() : nullptr,
				c.transition, c.poseWeight * c.maskWeight);

			Matrix actual = base;
			std::uint8_t driven = 0;
			const std::uint8_t displayedDriven = c.snapshot;
			const Bone bone{ 0, c.maskWeight };
			const Layer layer = MakeLayer(Mode::kOverride, c.transition, c.poseWeight,
				sampled, bone, &displayed, &displayedDriven);
			Check(Compose(&layer, 1, c.haveBase, actual.data(), &driven, 1), "depth 1 composes override");
			Check(std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
				"depth-1 override is byte-identical to WriteOverrideTransition");
			Check(driven == static_cast<std::uint8_t>(c.driven), "override ownership follows contribution and outgoing release");
		}

		Matrix expected = base;
		WriteOverrideTransition(expected.data(), base.data(), sampled.data(), nullptr, 0.4f, 0.5f);
		Matrix actual = base;
		std::uint8_t driven = 0;
		const Bone bone{ 0, 0.5f };
		const Layer layer = MakeLayer(Mode::kOverride, 0.4f, 1.0f, sampled, bone, &displayed);
		Check(Compose(&layer, 1, true, actual.data(), &driven, 1) &&
			std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
			"a snapshot without ownership flags cannot replace the base");
	}

	// L7.2: additive equality, exact no-ops, and base pad/scale preservation.
	{
		Matrix base = Transform(AxisAngle(0, 0, 1, kHalfPi), 1.0f, 2.0f, 3.0f, 1.25f);
		base[3] = 0.5f;
		base[7] = -0.25f;
		base[11] = 0.75f;
		const Matrix reference = Transform();
		const Matrix sampled = Transform(AxisAngle(1, 0, 0, kHalfPi), 0.0f, 1.0f, 0.0f, 2.0f);
		const Bone bone{ 0, 1.0f };
		for (const float weight : { 1.0f, 0.5f, 0.0f }) {
			Matrix expected = base;
			WriteAdditive(expected.data(), base.data(), reference.data(), sampled.data(), weight);
			Matrix actual = base;
			std::uint8_t driven = 0;
			const Layer layer = MakeAdditive(weight, sampled, reference, bone);
			Check(Compose(&layer, 1, true, actual.data(), &driven, 1), "depth 1 composes additive");
			Check(std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
				"depth-1 additive is byte-identical to WriteAdditive");
			for (const std::size_t index : { 3u, 7u, 11u, 15u }) {
				Check(std::memcmp(&actual[index], &base[index], sizeof(float)) == 0,
					"additive preserves each non-zero base pad and scale byte for byte");
			}
			Check(driven == static_cast<std::uint8_t>(weight > 0.0f), "zero additive weight releases ownership");
			if (weight == 0.0f) {
				Check(std::memcmp(actual.data(), base.data(), sizeof(Matrix)) == 0,
					"zero additive weight copies the base verbatim");
			}
		}

		Matrix noop = base;
		std::uint8_t driven = 0;
		const Layer rest = MakeAdditive(1.0f, reference, reference, bone);
		Check(Compose(&rest, 1, true, noop.data(), &driven, 1) &&
			std::memcmp(noop.data(), base.data(), sizeof(Matrix)) == 0,
			"a rest-pose additive sample is a byte-for-byte no-op through Compose");
		Check(driven == 1, "a positive-weight rest sample retains layer ownership");

		for (const float transition : { 0.4f, 1.5f, -0.5f }) {
			const Bone feathered{ 0, 0.5f };
			const Layer layer = MakeAdditive(1.5f, sampled, reference, feathered, transition);
			const float weight = EffectiveWeight(transition, 1.5f) * feathered.maskWeight;
			Matrix expected = base;
			WriteAdditive(expected.data(), base.data(), reference.data(), sampled.data(), weight);
			Matrix actual = base;
			driven = 0;
			Check(Compose(&layer, 1, true, actual.data(), &driven, 1) &&
				std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
				"additive combines clamped transition and pose weight before applying the mask");
			Check(driven == static_cast<std::uint8_t>(weight > 0.0f), "additive ownership uses the effective masked weight");
		}

		Matrix waiting = base;
		driven = 0;
		Layer layer = MakeAdditive(1.0f, sampled, reference, bone);
		Check(Compose(&layer, 1, false, waiting.data(), &driven, 1) && driven == 0 &&
			std::memcmp(waiting.data(), base.data(), sizeof(Matrix)) == 0,
			"additive waits for a proven base without writing or driving a joint");
		layer.reference = nullptr;
		Check(Compose(&layer, 1, true, waiting.data(), &driven, 1) && driven == 0 &&
			std::memcmp(waiting.data(), base.data(), sizeof(Matrix)) == 0,
			"additive without a reference inherits the pre-seeded base");
	}

	// L7.3: pre-seeding reaches the same bytes as today's untouched rig slot.
	{
		Matrix slot = Transform(AxisAngle(0, 1, 0, kHalfPi), 3.0f, 2.0f, 1.0f);
		const Matrix untouched = slot;
		const Matrix sampled = Transform();
		Check(!WriteOverrideTransition(slot.data(), untouched.data(), sampled.data(), nullptr, 0.0f, 1.0f) &&
			std::memcmp(slot.data(), untouched.data(), sizeof(Matrix)) == 0,
			"a declined override leaves the slot byte-identical");
		Matrix out = untouched;
		std::uint8_t driven = 0;
		const Bone bone{ 0, 1.0f };
		const Layer layer = MakeLayer(Mode::kOverride, 0.0f, 1.0f, sampled, bone);
		Check(Compose(&layer, 1, true, out.data(), &driven, 1) &&
			std::memcmp(out.data(), slot.data(), sizeof(Matrix)) == 0 && driven == 0,
			"pre-seeding and a declined compose inherit the untouched engine slot");
		Check(Compose(nullptr, 0, true, out.data(), &driven, 1) &&
			std::memcmp(out.data(), untouched.data(), sizeof(Matrix)) == 0 && driven == 0,
			"an empty stack inherits the base without driving any joint");
	}

	// L7.4: refuse the whole stack before the first layer can mutate either output.
	{
		const Matrix sampled = Transform({}, 9.0f, 8.0f, 7.0f);
		const Bone bone{ 0, 1.0f };
		const Layer layer = MakeLayer(Mode::kOverride, 1.0f, 1.0f, sampled, bone);
		const std::array<Layer, 3> stack{ layer, layer, layer };
		for (const std::size_t depth : { 2u, 3u }) {
			for (const std::uint8_t sentinel : { std::uint8_t{ 0 }, std::uint8_t{ 0xA5 } }) {
				Matrix out = Transform({}, 3.0f, 2.0f, 1.0f, 1.25f);
				const Matrix before = out;
				std::uint8_t driven = sentinel;
				Check(!Compose(stack.data(), depth, true, out.data(), &driven, 1),
					"the depth-1 composer refuses a stack deeper than one");
				Check(std::memcmp(out.data(), before.data(), sizeof(Matrix)) == 0 && driven == sentinel,
					"a refused stack leaves pose bytes and driven bytes untouched");
			}
		}
	}

	// L2 at depth 0: unlisted joints inherit base(0); buffers are addressed by joint, not mask order.
	{
		const Matrix unlisted = Transform(AxisAngle(0, 1, 0, kHalfPi), 9.0f, 8.0f, 7.0f, 1.75f);
		const Matrix base = Transform(AxisAngle(1, 0, 0, kHalfPi), 1.0f, 2.0f, 3.0f);
		const Matrix sampled = Transform(AxisAngle(0, 0, 1, kHalfPi), 4.0f, 5.0f, 6.0f);
		const Matrix reference = Transform({}, 0.0f, 1.0f);
		const Matrix displayed = Transform({}, 7.0f, 8.0f, 9.0f);
		std::array<float, 32> samples{}, references{}, snapshots{};
		std::copy(sampled.begin(), sampled.end(), samples.begin() + 16);
		std::copy(reference.begin(), reference.end(), references.begin() + 16);
		std::copy(displayed.begin(), displayed.end(), snapshots.begin() + 16);
		const std::array<std::uint8_t, 2> displayedDriven{ 0, 1 };
		const std::array<Bone, 2> bones{ Bone{ 1, 0.5f }, Bone{ 2, 1.0f } };
		for (const Mode mode : { Mode::kOverride, Mode::kAdditive }) {
			std::array<float, 32> out{};
			std::copy(unlisted.begin(), unlisted.end(), out.begin());
			std::copy(base.begin(), base.end(), out.begin() + 16);
			std::array<std::uint8_t, 2> driven{};
			const Layer layer{ mode, 0.5f, 0.4f, samples.data(), references.data(), snapshots.data(),
				displayedDriven.data(), bones.data(), bones.size() };
			Matrix expected = base;
			if (mode == Mode::kAdditive) {
				WriteAdditive(expected.data(), base.data(), reference.data(), sampled.data(), EffectiveWeight(0.4f, 0.5f) * 0.5f);
			} else {
				WriteOverrideTransition(expected.data(), base.data(), sampled.data(), displayed.data(), 0.4f, 0.25f);
			}
			Check(Compose(&layer, 1, true, out.data(), driven.data(), driven.size()), "a masked depth-1 layer composes");
			Check(std::memcmp(out.data(), unlisted.data(), sizeof(Matrix)) == 0 && driven[0] == 0,
				"an unlisted joint inherits the engine base byte for byte");
			Check(std::memcmp(out.data() + 16, expected.data(), sizeof(Matrix)) == 0 && driven[1] == 1,
				"the listed joint uses its own sample, reference, snapshot, and feathered weight");
		}
	}

	return Finish("Layer pose");
}
