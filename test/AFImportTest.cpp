// Offline harness: runs the .af -> ozz import pipeline on a clip + rig from the command line,
// without the game. Build with: xmake build osf-af-import-test
//   osf-af-import-test <clip.af> <skeleton.rig>
#include "Serialization/AFImport.h"

#include "Animation/OzzTypes.h"
#include "Util/Ba2.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/span.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 3) {
		std::printf("usage: osf-af-import-test <clip.af> <skeleton.rig | @ba2>\n");
		std::printf("  @ba2 reads the human rig from the game BA2s (run from the Starfield install dir)\n");
		return 2;
	}

	OSF::Serialization::AFLoadResult result;
	try {
		if (std::strcmp(argv[2], "@ba2") == 0) {
			result = OSF::Serialization::AFImport::LoadAnimation(argv[1], "human-skeleton", []() {
				return OSF::Util::Ba2::ReadGameFile("meshes/actors/human/characterassets/skeleton.rig");
			});
		} else {
			result = OSF::Serialization::AFImport::LoadAnimation(argv[1], argv[2]);
		}
	} catch (const std::exception& e) {
		std::printf("LoadAnimation threw: %s\n", e.what());
		return 1;
	} catch (...) {
		std::printf("LoadAnimation threw a non-std exception\n");
		return 1;
	}
	std::printf("error=%d detail=%s\n", static_cast<int>(result.error), result.detail.c_str());

	if (result.error != OSF::Serialization::AFError::kSuccess) {
		return 1;
	}

	const auto* skel = result.skeleton->data.get();
	const auto* anim = result.anim->data.get();
	std::printf("ok: joints=%d duration=%.3fs tracks=%d\n", skel->num_joints(), anim->duration(), anim->num_tracks());
	std::printf("first joints: ");
	for (int i = 0; i < skel->num_joints() && i < 8; i++) {
		std::printf("%s ", skel->joint_names()[i]);
	}
	std::printf("\n");

	// Sample the clip at several times and print a few joints' local transforms (x-basis +
	// translation of the unpacked matrix) — verifies the decoded animation varies over time.
	const char* watch[] = { "COM", "C_Hips", "L_Biceps", "C_Head" };
	std::vector<int> watchIdx;
	for (const char* w : watch) {
		for (int i = 0; i < skel->num_joints(); i++) {
			if (std::strcmp(skel->joint_names()[i], w) == 0) {
				watchIdx.push_back(i);
				break;
			}
		}
	}

	ozz::animation::SamplingJob::Context context;
	context.Resize(skel->num_joints());
	std::vector<ozz::math::SoaTransform> localPose(skel->num_soa_joints());
	std::vector<ozz::math::Float4x4> outputPose(skel->num_joints(), ozz::math::Float4x4::identity());

	for (float ratio : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		ozz::animation::SamplingJob job;
		job.animation = anim;
		job.context = &context;
		job.ratio = ratio;
		job.output = ozz::make_span(localPose);
		if (!job.Run()) {
			std::printf("sampling FAILED at ratio %.2f\n", ratio);
			return 1;
		}
		OSF::Animation::UnpackSoaTransforms({ localPose.data(), localPose.size() },
			{ outputPose.data(), outputPose.size() }, skel);

		std::printf("t=%5.2fs:", ratio * anim->duration());
		for (int idx : watchIdx) {
			float m[16];
			std::memcpy(m, &outputPose[idx], 64);
			std::printf("  %s xb=(%+.2f,%+.2f,%+.2f) t=(%+.2f,%+.2f,%+.2f)",
				skel->joint_names()[idx], m[0], m[1], m[2], m[12], m[13], m[14]);
		}
		std::printf("\n");
	}
	return 0;
}
