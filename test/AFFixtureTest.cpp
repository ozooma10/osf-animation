// Self-contained AF/rig/BA2 release regressions. No Bethesda assets or running game are needed.
#include "Check.h"
#include "Serialization/AFImport.h"
#include "Util/Ba2.h"

#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/span.h"
#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using Bytes = std::vector<std::uint8_t>;
	using OSF::Serialization::AFError;
	using OSF::Serialization::AFImport;
	using OSF::Serialization::AFLoadResult;
	using OSF::Test::Check;
	using Matrix = std::array<float, 16>;

	// All integer writes are explicitly little endian; offsets describe the on-disk format.
	void Put(Bytes& a_out, std::size_t a_offset, std::uint64_t a_value, std::size_t a_width)
	{
		if (a_out.size() < a_offset + a_width) a_out.resize(a_offset + a_width);
		for (std::size_t i = 0; i < a_width; ++i) a_out[a_offset + i] = static_cast<std::uint8_t>(a_value >> (8 * i));
	}
	void Float(Bytes& a_out, std::size_t a_offset, float a_value)
	{
		Put(a_out, a_offset, std::bit_cast<std::uint32_t>(a_value), 4);
	}
	void Align(Bytes& a_out, std::size_t a_alignment)
	{
		a_out.resize((a_out.size() + a_alignment - 1) / a_alignment * a_alignment);
	}
	void Append(Bytes& a_out, std::uint64_t a_value, std::size_t a_width)
	{
		Align(a_out, a_width);
		Put(a_out, a_out.size(), a_value, a_width);
	}
	void Text(Bytes& a_out, std::string_view a_text)
	{
		a_out.insert(a_out.end(), a_text.begin(), a_text.end());
	}
	void Write(const std::filesystem::path& a_path, const Bytes& a_bytes)
	{
		std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
		if (!file) throw std::runtime_error("could not write fixture: " + a_path.string());
	}

	class TempDirectory
	{
	public:
		TempDirectory() : original(std::filesystem::current_path())
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				auto candidate = std::filesystem::temp_directory_path() /
					("osf-af-fixtures-" + std::to_string(stamp) + "-" + std::to_string(attempt));
				if (std::filesystem::create_directory(candidate)) {
					path = std::move(candidate);
					std::filesystem::current_path(path);
					return;
				}
			}
			throw std::runtime_error("could not create isolated fixture directory");
		}
		~TempDirectory()
		{
			AFImport::ClearCache();
			std::error_code ec;
			std::filesystem::current_path(original, ec);
			std::filesystem::remove_all(path, ec);
		}
		std::filesystem::path path;
		std::filesystem::path original;
	};

	struct Bone
	{
		std::string name;
		std::int32_t parent;
		std::array<float, 3> position{};
		std::array<float, 4> rotation{ 1, 0, 0, 0 };  // rig uses WXYZ
	};
	Bytes Rig(const std::vector<Bone>& a_bones)
	{
		Bytes bytes(80 + 96 * a_bones.size());
		Put(bytes, 0, 1, 4);
		Put(bytes, 8, 80, 4);
		Float(bytes, 48, 0.5f);
		Float(bytes, 52, 0.125f);
		Put(bytes, 56, a_bones.size(), 2);
		Put(bytes, 58, a_bones.size(), 2);
		for (std::size_t i = 0; i < a_bones.size(); ++i) {
			const auto off = 80 + i * 96;
			for (std::size_t j = 0; j < 4; ++j) Float(bytes, off + j * 4, a_bones[i].rotation[j]);
			for (std::size_t j = 0; j < 3; ++j) Float(bytes, off + 32 + j * 4, a_bones[i].position[j]);
			Put(bytes, off + 48, bytes.size(), 8);
			Put(bytes, off + 56, static_cast<std::uint32_t>(a_bones[i].parent), 4);
			Text(bytes, a_bones[i].name);
			bytes.push_back(0);
		}
		Put(bytes, 4, bytes.size(), 4);
		return bytes;
	}
	Bytes ReorderedRig()
	{
		constexpr float q = 0.7071067811865475f;
		return Rig({ { "Arm", 2, { 10, 20, 30 }, { q, 0, 0, q } },
			{ "COM", -1, { 1, 2, 3 } }, { "Spine", 1, { 0, 1, 0 } } });
	}

	struct ClipOptions
	{
		bool wideCounters = false;
		bool wideKeys = false;
		bool preamble = false;
		bool ancillary = false;
		bool allSkipped = false;
		std::uint16_t lastFrame = 30;
		std::uint8_t rotationRepeats = 1;
		std::uint16_t translationRepeats = 2;
		std::array<std::uint8_t, 3> rotationPrefix{};
		std::array<std::uint8_t, 3> rotationEntry{};
		std::uint8_t missingComponent = 0;  // identity smallest-three values, missing X = 180 degrees about X
	};
	Bytes Clip(const ClipOptions& a_options = {})
	{
		Bytes bytes(64);
		const std::string_view magic{ "AF  ", 4 };
		std::copy(magic.begin(), magic.end(), bytes.begin());
		Float(bytes, 8, 1);
		bytes[36] = static_cast<std::uint8_t>((a_options.wideCounters ? 2 : 0) | (a_options.wideKeys ? 4 : 0));
		Put(bytes, 40, 1, 2);
		Put(bytes, 42, 3, 2);
		Put(bytes, 44, a_options.lastFrame + 1, 2);
		Put(bytes, 46, a_options.allSkipped ? 2 : 3, 2);
		Put(bytes, 48, a_options.preamble ? 1 : 0, 2);
		Put(bytes, 50, a_options.preamble ? 1 : 0, 2);
		if (a_options.preamble) {
			Append(bytes, 1, 4);  // one preamble entry
			Append(bytes, 1, 2);  // one channel
			for (int i = 0; i < 2; ++i) Append(bytes, std::bit_cast<std::uint32_t>(0.5f), 4);
			for (unsigned i = 0; i <= a_options.lastFrame; ++i) Append(bytes, 0, 4);
			Append(bytes, 4, 2);
			Append(bytes, 1, 1);
			Append(bytes, 0, 4);  // fill float
		}
		Align(bytes, 4);
		if (a_options.allSkipped) {
			bytes.insert(bytes.end(), { 3, 0 });
			return bytes;
		}
		bytes.insert(bytes.end(), { 0, 1, 2 });  // animate rig bone 0, preserve 1 and 2
		const auto counterWidth = a_options.wideCounters ? 2u : 1u;
		const auto keyWidth = a_options.wideKeys ? 2u : 1u;
		for (const auto count : { 2, 1, 2, 1, a_options.ancillary ? 1 : 0, a_options.ancillary ? 1 : 0 }) {
			Append(bytes, count, counterWidth);
		}
		for (int lane = 0; lane < 2; ++lane) {
			Append(bytes, 0, keyWidth);
			Append(bytes, a_options.lastFrame, keyWidth);
		}
		if (a_options.ancillary) {
			Append(bytes, 0, keyWidth);  // scalar and priority keys
			Append(bytes, 0, keyWidth);
		}
		for (int key = 0; key < 2; ++key) bytes.insert(bytes.end(), a_options.rotationEntry.begin(), a_options.rotationEntry.end());
		bytes.insert(bytes.end(), a_options.rotationPrefix.begin(), a_options.rotationPrefix.end());
		bytes.push_back(static_cast<std::uint8_t>(a_options.rotationRepeats | (a_options.missingComponent << 6)));
		bytes.insert(bytes.end(), { 0, 0, 0, 8, 248, 4 });  // signed fine translation deltas
		Append(bytes, 2, 2);
		Append(bytes, static_cast<std::uint16_t>(-4), 2);
		Append(bytes, 6, 2);
		Append(bytes, a_options.translationRepeats, 2);
		if (a_options.ancillary) {
			Append(bytes, 123, 2);  // scalar and priority values are intentionally ignored
			Append(bytes, 127, 1);
		}
		return bytes;
	}

	AFLoadResult Load(const Bytes& a_clip, const Bytes& a_rig, std::string_view a_key = "fixture")
	{
		return AFImport::LoadAnimation(a_key, a_clip, "fixture-rig", [&] { return std::optional{ a_rig }; });
	}
	bool Success(const AFLoadResult& a_result)
	{
		return Check(a_result.error == AFError::kSuccess && a_result.skeleton && a_result.skeleton->data &&
			a_result.anim && a_result.anim->data, "AF import succeeds: " + a_result.detail);
	}
	void Failure(const AFLoadResult& a_result, AFError a_expected)
	{
		Check(a_result.error == a_expected, "expected error " + std::to_string(static_cast<int>(a_expected)) +
			", got " + std::to_string(static_cast<int>(a_result.error)) + ": " + a_result.detail);
		Check(!a_result.skeleton && !a_result.anim, "failed imports publish no partial playback objects");
		Check(!a_result.detail.empty(), "failed imports include a diagnostic");
	}
	void Near(float a_value, float a_expected, std::string_view a_label)
	{
		Check(std::isfinite(a_value) && std::abs(a_value - a_expected) < 0.004f,
			std::string(a_label) + ": got " + std::to_string(a_value) + ", expected " + std::to_string(a_expected));
	}
	std::vector<Matrix> Sample(const AFLoadResult& a_result, float a_ratio)
	{
		const auto* skeleton = a_result.skeleton->data.get();
		ozz::animation::SamplingJob::Context context(skeleton->num_joints());
		std::vector<ozz::math::SoaTransform> local(skeleton->num_soa_joints());
		std::vector<ozz::math::Float4x4> matrices(skeleton->num_joints());
		ozz::animation::SamplingJob job;
		job.animation = a_result.anim->data.get();
		job.context = &context;
		job.ratio = a_ratio;
		job.output = ozz::make_span(local);
		if (!job.Run()) throw std::runtime_error("ozz sampling failed");
		OSF::Animation::UnpackSoaTransforms(local, matrices, skeleton);
		std::vector<Matrix> result(matrices.size());
		for (std::size_t i = 0; i < matrices.size(); ++i) std::memcpy(result[i].data(), &matrices[i], sizeof(Matrix));
		return result;
	}
	void VerifyMotion(const AFLoadResult& a_result, float a_duration = 1.0f)
	{
		if (!Success(a_result)) return;
		const auto& skeleton = *a_result.skeleton->data;
		if (!Check(skeleton.num_joints() == 3 && a_result.anim->data->num_tracks() == 3, "three joints and tracks imported")) return;
		const std::array names{ "COM", "Spine", "Arm" };
		for (int i = 0; i < 3; ++i) {
			Check(std::string_view(skeleton.joint_names()[i]) == names[i], "rig reordered into hierarchy order");
			Check(skeleton.joint_parents()[i] == i - 1, "parent indices follow reordered hierarchy");
		}
		Near(a_result.anim->data->duration(), a_duration, "AF duration follows 30 fps keys");
		for (float ratio : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			const auto pose = Sample(a_result, ratio);
			Near(pose[0][12], 1, "atlas-skipped root retains bind X");
			Near(pose[0][13], 2, "atlas-skipped root retains bind Y");
			Near(pose[0][14], 3, "atlas-skipped root retains bind Z");
			Near(pose[1][13], 1, "atlas-skipped child retains local bind translation");
			Near(pose[2][12], 12 + ratio, "rest-relative track rotates into bind basis X");
			Near(pose[2][13], 21 + ratio, "signed coarse/fine translation interpolates Y");
			Near(pose[2][14], 33 + 0.5f * ratio, "signed coarse/fine translation interpolates Z");
			Near(pose[2][0], 0, "rotation track composes with bind rotation");
			Near(pose[2][1], 1, "composed rotation X axis");
			Near(pose[2][4], 1, "composed rotation Y axis");
			Near(pose[2][10], -1, "smallest-three missing X reconstructed");
		}
	}

	Bytes Zlib(const Bytes& a_input)
	{
		uLongf size = compressBound(static_cast<uLong>(a_input.size()));
		Bytes bytes(size);
		if (compress2(bytes.data(), &size, a_input.data(), static_cast<uLong>(a_input.size()), Z_BEST_COMPRESSION) != Z_OK) {
			throw std::runtime_error("fixture compression failed");
		}
		bytes.resize(size);
		return bytes;
	}
	struct ArchiveFile
	{
		std::string name;
		Bytes bytes;
		bool compressed = false;
	};
	Bytes Archive(std::uint32_t a_version, const std::vector<ArchiveFile>& a_files)
	{
		const auto header = a_version >= 2 ? 32u : 24u;
		Bytes bytes(header + a_files.size() * 36);
		std::memcpy(bytes.data(), "BTDX", 4);
		Put(bytes, 4, a_version, 4);
		std::memcpy(bytes.data() + 8, "GNRL", 4);
		Put(bytes, 12, a_files.size(), 4);
		for (std::size_t i = 0; i < a_files.size(); ++i) {
			const auto stored = a_files[i].compressed ? Zlib(a_files[i].bytes) : a_files[i].bytes;
			const auto record = header + i * 36;
			Put(bytes, record + 16, bytes.size(), 8);
			Put(bytes, record + 24, a_files[i].compressed ? stored.size() : 0, 4);
			Put(bytes, record + 28, a_files[i].bytes.size(), 4);
			bytes.insert(bytes.end(), stored.begin(), stored.end());
		}
		Put(bytes, 16, bytes.size(), 8);
		for (const auto& file : a_files) {
			// Name-table lengths are packed, without alignment between entries.
			Put(bytes, bytes.size(), file.name.size(), 2);
			Text(bytes, file.name);
		}
		return bytes;
	}
	constexpr std::string_view kRigPath = "meshes/actors/human/characterassets/skeleton.rig";
	void Run(std::string_view a_name, const std::function<void()>& a_test)
	{
		std::cout << "AF/BA2 fixtures: " << a_name << '\n';
		AFImport::ClearCache();
		try {
			a_test();
		} catch (const std::exception& error) {
			Check(false, std::string(a_name) + " threw: " + error.what());
		} catch (...) {
			Check(false, std::string(a_name) + " threw a non-standard exception");
		}
	}
}

int main()
{
	Run("all counter/key widths, hierarchy, signed translations and bind composition", [] {
		for (bool counters : { false, true }) {
			for (bool keys : { false, true }) {
				AFImport::ClearCache();
				ClipOptions options;
				options.wideCounters = counters;
				options.wideKeys = keys;
				options.lastFrame = keys ? 300 : 30;
				VerifyMotion(Load(Clip(options), ReorderedRig()), keys ? 10.0f : 1.0f);
			}
		}
	});
	Run("preamble, padding, scalar and priority lanes", [] {
		ClipOptions options;
		options.preamble = true;
		options.ancillary = true;
		VerifyMotion(Load(Clip(options), ReorderedRig()));
	});
	Run("RLE counts are bounded by actual keys", [] {
		ClipOptions options;
		options.rotationRepeats = 63;
		options.translationRepeats = 65535;
		VerifyMotion(Load(Clip(options), ReorderedRig()));
	});
	Run("all-skipped atlas produces a bind-pose clip", [] {
		ClipOptions options;
		options.allSkipped = true;
		const auto result = Load(Clip(options), ReorderedRig());
		if (!Success(result)) return;
		Near(result.anim->data->duration(), 1.0f / 30, "static clip retains positive minimum duration");
		for (float ratio : { 0.0f, 1.0f }) {
			const auto pose = Sample(result, ratio);
			Near(pose[2][12], 10, "unanimated arm bind translation retained");
			Near(pose[2][1], 1, "unanimated arm bind rotation retained");
			Near(pose[2][10], 1, "unanimated arm has no rest-relative rotation applied");
		}
	});
	Run("all quaternion dropped components and signed precision flags", [] {
		for (std::uint8_t missing = 0; missing < 4; ++missing) {
			AFImport::ClearCache();
			ClipOptions options;
			options.missingComponent = missing;
			options.rotationPrefix = { 0xfc, 4, 0 };  // signed -4 with precision flag, +4, 0
			options.rotationEntry = { 248, 8, 0 };  // signed -8, +8, 0
			const auto result = Load(Clip(options), Rig({ { "Arm", -1 } }));
			if (!Success(result)) continue;
			constexpr float low = 0.7071067811865475f / 64;
			constexpr float high = 0.7071067811865475f / 16384;
			const float first = -4 * low - 8 * 8 * high;
			const float second = 4 * low + 8 * high;
			std::array<float, 4> q{};
			const std::array remaining{ first, second, 0.0f };
			for (unsigned i = 0, entry = 0; i < 4; ++i) {
				q[i] = i == missing ? std::sqrt(1 - first * first - second * second) : remaining[entry++];
			}
			const auto pose = Sample(result, 0.5f);
			Near(pose[0][0], 1 - 2 * (q[1] * q[1] + q[2] * q[2]), "quaternion decoded diagonal X");
			Near(pose[0][1], 2 * (q[0] * q[1] + q[2] * q[3]), "quaternion decoded off-diagonal XY");
			Near(pose[0][6], 2 * (q[1] * q[2] + q[0] * q[3]), "quaternion decoded off-diagonal YZ");
		}
	});
	Run("provider caching, identities and reload invalidation", [] {
		int reads = 0;
		auto provider = [&]() -> std::optional<Bytes> { ++reads; return ReorderedRig(); };
		const auto first = AFImport::LoadAnimation("CLIP.AF", Clip(), "rig-A", provider);
		const auto again = AFImport::LoadAnimation("clip.af", Bytes{}, "rig-A", provider);
		Check(Success(first) && Success(again) && first.anim == again.anim && first.skeleton == again.skeleton,
			"clip identity is case insensitive and cache shares immutable playback objects");
		const auto other = AFImport::LoadAnimation("other.af", Clip(), "rig-A", provider);
		Check(Success(other) && other.skeleton == first.skeleton && reads == 1, "different clips fetch a shared rig once");
		const auto otherRig = AFImport::LoadAnimation("clip.af", Clip(), "rig-B", provider);
		Check(Success(otherRig) && otherRig.skeleton != first.skeleton && reads == 2, "rig identity isolates cache entries");
		AFImport::ClearCache();
		const auto reloaded = AFImport::LoadAnimation("clip.af", Clip(), "rig-A", provider);
		Check(Success(reloaded) && reloaded.anim != first.anim && reloaded.skeleton != first.skeleton && reads == 3,
			"reload clears both caches and fetches rig again");
		VerifyMotion(first);  // pinned playback references survive invalidation
	});
	Run("failed reads are retryable and do not invoke unnecessary providers", [] {
		int reads = 0;
		auto unavailable = [&]() -> std::optional<Bytes> { ++reads; return std::nullopt; };
		Failure(AFImport::LoadAnimation("retry", Clip(), "rig", unavailable), AFError::kRigReadFailed);
		Failure(AFImport::LoadAnimation("retry", Clip(), "rig", {}), AFError::kRigReadFailed);
		auto available = [&]() -> std::optional<Bytes> { ++reads; return ReorderedRig(); };
		VerifyMotion(AFImport::LoadAnimation("retry", Clip(), "rig", available));
		Check(reads == 2, "unavailable rig is not negatively cached");
		TempDirectory fixture;
		Failure(AFImport::LoadAnimation(fixture.path / "missing.af", "other-rig", available), AFError::kAfReadFailed);
		Check(reads == 2, "missing AF file fails before reading the rig");
		Write(fixture.path / "clip.af", Clip());
		Write(fixture.path / "skeleton.rig", ReorderedRig());
		VerifyMotion(AFImport::LoadAnimation(fixture.path / "clip.af", fixture.path / "skeleton.rig"));
	});
	Run("AF truncation at every byte boundary", [] {
		const auto full = Clip();
		const auto rig = ReorderedRig();
		for (std::size_t n = 0; n < full.size(); ++n) {
			Failure(Load(Bytes(full.begin(), full.begin() + n), rig, "truncated-" + std::to_string(n)), AFError::kAfParseFailed);
		}
		VerifyMotion(Load(full, rig, "repaired"));
	});
	Run("rig structure, bounds and cyclic hierarchy rejection", [] {
		auto rejects = [](Bytes bytes, AFError expected) {
			AFImport::ClearCache();
			Failure(Load(Clip(), bytes), expected);
		};
		const auto rig = ReorderedRig();
		for (std::size_t n : { 0u, 1u, 56u, 79u, 80u, 175u, 367u }) {
			rejects(Bytes(rig.begin(), rig.begin() + n), AFError::kRigParseFailed);
		}
		auto bad = rig;
		Put(bad, 56, 0, 2);
		rejects(bad, AFError::kRigParseFailed);
		bad = rig;
		Put(bad, 56, 4097, 2);
		rejects(bad, AFError::kRigParseFailed);
		bad = rig;
		Put(bad, 80 + 48, rig.size() + 20, 8);
		rejects(bad, AFError::kRigParseFailed);
		rejects(Rig({ { "bad-parent", 2 } }), AFError::kFailedToBuildSkeleton);
		rejects(Rig({ { "self-cycle", 0 } }), AFError::kFailedToBuildSkeleton);
		rejects(Rig({ { "root", -1 }, { "cycle-a", 2 }, { "cycle-b", 1 } }), AFError::kFailedToBuildSkeleton);
		std::vector<Bone> deep;
		for (int i = 0; i < 513; ++i) deep.push_back({ "bone-" + std::to_string(i), i - 1 });
		rejects(Rig(deep), AFError::kFailedToBuildSkeleton);
	});
	Run("invalid AF atlas and nonmonotonic keys", [] {
		for (auto [offset, value] : std::array{ std::pair{ 42u, 0u }, std::pair{ 46u, 1u }, std::pair{ 65u, 2u } }) {
			auto bytes = Clip();
			bytes[offset] = static_cast<std::uint8_t>(value);
			Failure(Load(bytes, ReorderedRig(), "invalid-" + std::to_string(offset)), AFError::kAfParseFailed);
		}
		auto bytes = Clip();
		bytes[74] = 0;  // duplicate rotation frame keys (counter block starts at byte 67)
		Failure(Load(bytes, ReorderedRig(), "duplicate-keys"), AFError::kFailedToMakeClip);
	});
	Run("BA2 v1/v2 raw/zlib lookup and rig provider integration", [] {
		TempDirectory fixture;
		Check(!OSF::Util::Ba2::ReadGameFile(kRigPath), "missing Data directory returns unavailable");
		std::filesystem::create_directory("Data");
		const auto rig = ReorderedRig();
		for (unsigned version : { 1u, 2u }) {
			for (bool compressed : { false, true }) {
				AFImport::ClearCache();
				Write("Data/fixture.ba2", Archive(version, { { "unrelated/odd", { 7, 8 }, false },
					{ "MESHES\\Actors\\Human\\CharacterAssets\\Skeleton.RIG", rig, compressed } }));
				const auto bytes = OSF::Util::Ba2::ReadGameFile(kRigPath, "fixture.ba2");
				Check(bytes && *bytes == rig, "archive finds second entry with normalized case and separators");
				VerifyMotion(AFImport::LoadAnimation("archive-clip", Clip(), "archive-rig", [] {
					return OSF::Util::Ba2::ReadGameFile(kRigPath, "fixture.ba2");
				}));
				Check(!OSF::Util::Ba2::ReadGameFile("meshes/absent.rig"), "missing archive entry returns unavailable");
			}
		}
	});
	Run("BA2 preferred archive, fallback, empty entries and non-general archives", [] {
		TempDirectory fixture;
		std::filesystem::create_directory("Data");
		Write("Data/first.ba2", Archive(2, { { "entry", { 1 }, false } }));
		Write("Data/preferred.BA2", Archive(2, { { "entry", { 2 }, false }, { "empty", {}, false } }));
		Check(OSF::Util::Ba2::ReadGameFile("entry", "preferred.BA2") == std::optional<Bytes>{ Bytes{ 2 } },
			"preferred archive takes precedence over directory enumeration");
		Check(OSF::Util::Ba2::ReadGameFile("empty", "missing.ba2") == std::optional<Bytes>{ Bytes{} },
			"missing preferred archive falls back and empty entries remain present");
		auto texture = Archive(2, { { "texture-only", { 9 }, false } });
		std::memcpy(texture.data() + 8, "DX10", 4);
		Write("Data/textures.ba2", texture);
		Write("Data/ignored.txt", Archive(2, { { "text-only", { 9 }, false } }));
		Check(!OSF::Util::Ba2::ReadGameFile("texture-only"), "DX10 archives are skipped");
		Check(!OSF::Util::Ba2::ReadGameFile("text-only"), "fallback scans only BA2 files");
	});
	Run("BA2 malformed header, names, offsets, sizes and zlib streams", [] {
		TempDirectory fixture;
		std::filesystem::create_directory("Data");
		const auto raw = Archive(2, { { "entry", { 1, 2, 3, 4 }, false } });
		auto rejects = [](const Bytes& bytes) {
			Write("Data/bad.ba2", bytes);
			Check(!OSF::Util::Ba2::ReadGameFile("entry", "bad.ba2"), "corrupt archive returns unavailable");
		};
		for (std::size_t n : { 0u, 4u, 16u, 24u, 35u, 67u, 73u }) rejects(Bytes(raw.begin(), raw.begin() + n));
		auto bad = raw;
		bad[0] = '!';
		rejects(bad);
		for (auto [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 7>{ {
			{ 12, 0, 4 }, { 16, raw.size() + 100, 8 }, { 48, raw.size() + 1, 8 },
			{ 56, 64u * 1024 * 1024 + 1, 4 }, { 60, 64u * 1024 * 1024 + 1, 4 },
			{ 60, 32, 4 }, { 72, 65535, 2 }
		} }) {
			bad = raw;
			Put(bad, static_cast<std::size_t>(offset), value, static_cast<std::size_t>(width));
			rejects(bad);
		}
		bad = Archive(2, { { "entry", Bytes(32, 7), true } });
		bad[68] = 0xff;  // invalid zlib header
		rejects(bad);
		bad = Archive(2, { { "entry", Bytes(32, 7), true } });
		Put(bad, 60, 33, 4);
		rejects(bad);  // decompressed length must exactly match the record
		Write("Data/recovery.ba2", Archive(2, { { "entry", { 42 }, false } }));
		Check(OSF::Util::Ba2::ReadGameFile("entry", "bad.ba2") == std::optional<Bytes>{ Bytes{ 42 } },
			"invalid preferred archive does not block a valid fallback");
	});
	AFImport::ClearCache();
	return OSF::Test::Finish("AF/BA2 fixtures");
}
