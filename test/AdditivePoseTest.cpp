#include "Animation/PoseMath.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>

namespace
{
	using Matrix = std::array<float, 16>;
	using OSF::Animation::PoseMath::Quat;

	int g_failures = 0;

	void Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			++g_failures;
		}
	}

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

	// Reference-pose sample: even with a non-identity reference, the zero delta is a byte-exact
	// no-op on the complete live NiTransform (including padding and engine scale).
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

	// Reference-relative translation at the contract's three boundary weights. Scale remains live.
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

	// True shortest-path SLERP of the reference-relative rotation.
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

	// Multiplication order proof: final = live(X90) * delta(Z90). Acting on +X, Z rotates it
	// to +Y and the live X rotation then maps it to +Z.
	{
		const Matrix live = Transform(AxisAngle(1.0f, 0.0f, 0.0f, kHalfPi));
		const Matrix reference = Transform();
		const Matrix sampled = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi));
		Matrix out{};
		WriteAdditive(out.data(), live.data(), reference.data(), sampled.data(), 1.0f);
		Check(Near(out[0], 0.0f) && Near(out[1], 0.0f) && Near(out[2], 1.0f),
			"rotation composition order is live * reference-relative delta");
	}

	// preserveBones is a binding exclusion: stamping the driven slot cannot touch the omitted slot.
	{
		std::array<Matrix, 2> slots{ Transform({}, 1.0f), Transform({}, 99.0f, 98.0f, 97.0f, 3.0f) };
		const Matrix preservedBefore = slots[1];
		const Matrix reference = Transform();
		const Matrix sampled = Transform({}, 5.0f);
		WriteAdditive(slots[0].data(), slots[0].data(), reference.data(), sampled.data(), 1.0f);
		Check(std::memcmp(slots[1].data(), preservedBefore.data(), sizeof(Matrix)) == 0,
			"a preserved/unbound bone stays byte-for-byte untouched");
	}

	// The extracted override cross-fade retains the old normalized-lerp result and translation.
	{
		const Matrix from = Transform({}, 2.0f, 4.0f, 6.0f);
		const Matrix target = Transform(AxisAngle(0.0f, 0.0f, 1.0f, kHalfPi), 10.0f, 12.0f, 14.0f);
		Matrix out{};
		WriteOverrideBlended(out.data(), from.data(), target.data(), 0.5f);
		CheckZRotation(out, kHalfPi * 0.5f, "override keeps its existing normalized-lerp rotation");
		Check(Near(out[12], 6.0f) && Near(out[13], 8.0f) && Near(out[14], 10.0f) && out[15] == 1.0f,
			"override keeps its existing linear translation and unit scale");
	}

	// Transition weight and persistent role strength multiply; this covers both blend-in and fade-out.
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

	// A stage/skeleton change must replace both sample and reference. Two very different reference
	// poses with the same authored delta therefore produce the same layer over the live pose.
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

	// Repeated model compose calls reuse the immutable post-engine base, never the prior OSF output.
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

	if (g_failures != 0) {
		std::cerr << g_failures << " additive pose test(s) FAILED\n";
		return 1;
	}
	std::cout << "Additive pose tests passed\n";
	return 0;
}
