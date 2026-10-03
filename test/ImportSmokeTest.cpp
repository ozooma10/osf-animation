// Import/sampling regression suite; all clip bytes are generated here, no game assets needed.
#include "Check.h"
#include "Serialization/GLTFImport.h"

#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/span.h"
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using OSF::Serialization::GLTFError;
	using OSF::Serialization::GLTFImport;
	using OSF::Serialization::LoadResult;
	using OSF::Test::Check;
	using Bytes = std::vector<std::byte>;
	using Json = nlohmann::json;

	// Atomic creation isolates concurrent runs.
	class TempDirectory
	{
	public:
		TempDirectory()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				auto candidate = std::filesystem::temp_directory_path() /
					("osf-import-smoke-" + std::to_string(stamp) + "-" + std::to_string(attempt));
				if (std::filesystem::create_directory(candidate)) {
					path = std::move(candidate);
					return;
				}
			}
			throw std::runtime_error("could not create an isolated fixture directory");
		}

		~TempDirectory()
		{
			GLTFImport::ClearCache();
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}

		std::filesystem::path path;
	};

	void AppendU32(Bytes& a_bytes, std::uint32_t a_value)
	{
		for (unsigned shift = 0; shift < 32; shift += 8) {
			a_bytes.push_back(static_cast<std::byte>((a_value >> shift) & 0xff));
		}
	}

	Bytes TextBytes(std::string_view a_text)
	{
		const auto* first = reinterpret_cast<const std::byte*>(a_text.data());
		return { first, first + a_text.size() };
	}

	void Write(const std::filesystem::path& a_path, const Bytes& a_bytes)
	{
		std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
		if (!file) {
			throw std::runtime_error("failed to write fixture: " + a_path.string());
		}
	}

	struct Fixture
	{
		Json doc;
		Bytes binary;

		std::size_t Accessor(std::initializer_list<float> a_values, std::string_view a_type, std::size_t a_count)
		{
			const auto offset = binary.size();
			for (float value : a_values) {
				AppendU32(binary, std::bit_cast<std::uint32_t>(value));
			}
			const auto view = doc["bufferViews"].size();
			doc["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", offset }, { "byteLength", binary.size() - offset } });
			const auto accessor = doc["accessors"].size();
			doc["accessors"].push_back({ { "bufferView", view }, { "componentType", 5126 },
				{ "count", a_count }, { "type", a_type } });
			return accessor;
		}

		Bytes Glb() const
		{
			auto json = doc;
			json["buffers"] = Json::array({ { { "byteLength", binary.size() } } });
			auto text = json.dump();
			text.resize((text.size() + 3) & ~std::size_t{ 3 }, ' ');
			Bytes bytes;
			AppendU32(bytes, 0x46546c67);  // glTF
			AppendU32(bytes, 2);
			AppendU32(bytes, static_cast<std::uint32_t>(28 + text.size() + binary.size()));
			AppendU32(bytes, static_cast<std::uint32_t>(text.size()));
			AppendU32(bytes, 0x4e4f534a);  // JSON
			const auto jsonBytes = TextBytes(text);
			bytes.insert(bytes.end(), jsonBytes.begin(), jsonBytes.end());
			AppendU32(bytes, static_cast<std::uint32_t>(binary.size()));
			AppendU32(bytes, 0x004e4942);  // BIN
			bytes.insert(bytes.end(), binary.begin(), binary.end());
			return bytes;
		}
	};

	Fixture MakeFixture(float a_translationOffset = 0.0f)
	{
		Fixture fixture;
		fixture.doc = Json::parse(R"({
			"asset":{"version":"2.0"},
			"nodes":[
				{"name":"HumanExportRoot","translation":[100,200,300],"children":[1]},
				{"name":"COM","children":[2]},
				{"name":"C_Hips","translation":[0,1,0],"children":[3]},
				{"name":"C_Head","translation":[0,0,0.25]}
			],
			"bufferViews":[],"accessors":[],"animations":[]
		})");
		const auto times = fixture.Accessor({ 0, 2 }, "SCALAR", 2);
		const auto motion = fixture.Accessor({ a_translationOffset, 1, 2, a_translationOffset + 2, 3, 4 }, "VEC3", 2);
		const auto alternate = fixture.Accessor({ 6, 1, 2, 10, 3, 4 }, "VEC3", 2);
		const auto placement = fixture.Accessor({ 100, 200, 300, 110, 220, 330 }, "VEC3", 2);
		const auto rotation = fixture.Accessor({ 0, 0, 0, 1, 0, 0, 1, 0 }, "VEC4", 2);
		const auto scale = fixture.Accessor({ 1, 1, 1, 3, 3, 3 }, "VEC3", 2);
		for (const auto& [name, translation] : std::array{
			std::pair{ "primary", motion }, std::pair{ "alternate", alternate } }) {
			Json animation{ { "name", name }, { "samplers", Json::array() }, { "channels", Json::array() } };
			const auto channel = [&](std::size_t a_output, unsigned a_node, const char* a_path) {
				const auto sampler = animation["samplers"].size();
				animation["samplers"].push_back({ { "input", times }, { "output", a_output }, { "interpolation", "LINEAR" } });
				animation["channels"].push_back({ { "sampler", sampler }, { "target", { { "node", a_node }, { "path", a_path } } } });
			};
			channel(translation, 1, "translation");
			channel(placement, 0, "translation");
			channel(rotation, 0, "rotation");
			channel(scale, 1, "scale");
			fixture.doc["animations"].push_back(std::move(animation));
		}
		return fixture;
	}

	Bytes Gzip(const Bytes& a_input)
	{
		z_stream stream{};
		if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
			throw std::runtime_error("gzip fixture initialization failed");
		}
		Bytes result(deflateBound(&stream, static_cast<uLong>(a_input.size())));
		stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(a_input.data()));
		stream.avail_in = static_cast<uInt>(a_input.size());
		stream.next_out = reinterpret_cast<Bytef*>(result.data());
		stream.avail_out = static_cast<uInt>(result.size());
		const auto status = deflate(&stream, Z_FINISH);
		result.resize(stream.total_out);
		deflateEnd(&stream);
		if (status != Z_STREAM_END) {
			throw std::runtime_error("gzip fixture compression failed");
		}
		return result;
	}

	bool Success(const LoadResult& a_result)
	{
		return Check(a_result.error == GLTFError::kSuccess && a_result.skeleton && a_result.skeleton->data &&
			a_result.anim && a_result.anim->data, "import succeeded: " + a_result.detail);
	}

	void Near(float a_value, float a_expected, std::string_view a_label)
	{
		// ozz stores compressed runtime keys, so allow their quantization error.
		Check(std::isfinite(a_value) && std::abs(a_value - a_expected) < 0.003f, a_label);
	}

	using Matrix = std::array<float, 16>;
	std::vector<Matrix> Sample(const LoadResult& a_result, float a_ratio)
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
		if (!job.Run()) {
			throw std::runtime_error("ozz sampling failed");
		}
		OSF::Animation::UnpackSoaTransforms(local, matrices, skeleton);
		std::vector<Matrix> result(matrices.size());
		for (std::size_t i = 0; i < matrices.size(); ++i) {
			std::memcpy(result[i].data(), &matrices[i], sizeof(Matrix));
		}
		return result;
	}

	void VerifyPrimary(const LoadResult& a_result)
	{
		if (!Success(a_result)) {
			return;
		}
		const auto& skeleton = *a_result.skeleton->data;
		if (!Check(skeleton.num_joints() == 4, "node hierarchy produces four joints")) {
			return;
		}
		Check(a_result.anim->data->num_tracks() == 4, "clip has one track per joint");
		Near(a_result.anim->data->duration(), 2.0f, "duration follows last key time");
		const std::array names{ "HumanExportRoot", "COM", "C_Hips", "C_Head" };
		const std::array parents{ -1, 0, 1, 2 };
		for (std::size_t i = 0; i < names.size(); ++i) {
			Check(std::string_view(skeleton.joint_names()[i]) == names[i], "joint order and names preserved");
			Check(skeleton.joint_parents()[i] == parents[i], "joint parent hierarchy preserved");
		}
		for (const auto ratio : { 0.0f, 0.5f, 1.0f }) {
			const auto pose = Sample(a_result, ratio);
			Near(pose[0][12], 0, "export-root animated X placement removed");
			Near(pose[0][13], 0, "export-root animated Y placement removed");
			Near(pose[0][14], 0, "export-root animated Z placement removed");
			Near(pose[1][12], 2 * ratio, "COM translation X interpolates");
			Near(pose[1][13], 1 + 2 * ratio, "COM translation Y interpolates");
			Near(pose[1][14], 2 + 2 * ratio, "COM translation Z interpolates");
			Near(pose[1][0], 1 + 2 * ratio, "animated scale interpolates");
			Near(pose[2][13], 1, "unkeyed hips retain rest translation when COM is present");
			Near(pose[3][14], 0.25f, "unkeyed descendant retains local rest pose");
		}
		const auto midpoint = Sample(a_result, 0.5f);
		Near(midpoint[0][0], 0, "root rotation is preserved despite dropped placement");
		Near(midpoint[0][1], 1, "root rotates 90 degrees around Z at midpoint");
	}

	void Failure(const LoadResult& a_result, GLTFError a_error)
	{
		Check(a_result.error == a_error, "invalid input error=" + std::to_string(static_cast<int>(a_result.error)) +
			", expected=" + std::to_string(static_cast<int>(a_error)) + ": " + a_result.detail);
		Check(!a_result.skeleton && !a_result.anim, "failed imports publish no partial playback objects");
		Check(!a_result.detail.empty(), "failed imports explain the failure");
	}

	void Run(std::string_view a_name, const std::function<void()>& a_case)
	{
		std::cout << "import smoke: " << a_name << '\n';
		GLTFImport::ClearCache();
		try {
			a_case();
		} catch (const std::exception& error) {
			Check(false, std::string(a_name) + " threw: " + error.what());
		} catch (...) {
			Check(false, std::string(a_name) + " threw a non-standard exception");
		}
	}
}

int main()
{
	try {
		TempDirectory temp;
		const auto fixture = MakeFixture();
		const auto glb = fixture.Glb();
		const auto glbPath = temp.path / "clip.glb";
		Write(glbPath, glb);

		Run("GLB hierarchy and numeric sampling", [&] {
			VerifyPrimary(GLTFImport::LoadAnimation(glbPath, ""));
		});
		Run("text glTF with external buffer", [&] {
			auto doc = fixture.doc;
			doc["buffers"] = Json::array({ { { "byteLength", fixture.binary.size() }, { "uri", "clip.bin" } } });
			Write(temp.path / "clip.bin", fixture.binary);
			Write(temp.path / "clip.gltf", TextBytes(doc.dump()));
			VerifyPrimary(GLTFImport::LoadAnimation(temp.path / "clip.gltf", "primary"));
		});
		Run("NAF gzip and memory acquisition", [&] {
			const auto compressed = Gzip(glb);
			Write(temp.path / "compressed.glb", compressed);
			VerifyPrimary(GLTFImport::LoadAnimation(temp.path / "compressed.glb", ""));
			VerifyPrimary(GLTFImport::LoadAnimation("memory-glb", glb, temp.path, ""));
			VerifyPrimary(GLTFImport::LoadAnimation("memory-gzip", compressed, temp.path, ""));
		});
		Run("animation names, indices and invalid identifiers", [&] {
			for (const auto id : { "alternate", "1" }) {
				const auto selected = GLTFImport::LoadAnimation(glbPath, id);
				if (Success(selected)) {
					Near(Sample(selected, 0.5f)[1][12], 8, "selector chooses second animation motion");
				}
			}
			VerifyPrimary(GLTFImport::LoadAnimation(glbPath, "0"));
			for (const auto id : { "missing", "Primary", "2", "-1", "999999999999999999999999999999999999" }) {
				Failure(GLTFImport::LoadAnimation(glbPath, id), GLTFError::kInvalidAnimationIdentifier);
			}
		});
		Run("cache identity and reload", [&] {
			const auto first = GLTFImport::LoadAnimation(glbPath, "primary");
			const auto normalized = GLTFImport::LoadAnimation(temp.path / "." / "clip.glb", "primary");
			Check(Success(first) && normalized.anim == first.anim && normalized.skeleton == first.skeleton,
				"normalized path reuses immutable cached objects");
			Write(glbPath, MakeFixture(20).Glb());
			const auto cached = GLTFImport::LoadAnimation(glbPath, "primary");
			Check(cached.anim == first.anim, "cached result survives on-disk changes until reload");
			GLTFImport::ClearCache();
			const auto reloaded = GLTFImport::LoadAnimation(glbPath, "primary");
			if (Success(reloaded)) {
				Check(reloaded.anim != first.anim, "ClearCache rebuilds changed clips");
				Near(Sample(reloaded, 0.5f)[1][12], 21, "reload samples replacement bytes");
			}
			Write(glbPath, glb);
			Failure(GLTFImport::LoadAnimation(temp.path / "created-later.glb", ""), GLTFError::kFileReadFailed);
			Write(temp.path / "created-later.glb", glb);
			VerifyPrimary(GLTFImport::LoadAnimation(temp.path / "created-later.glb", ""));
		});
		Run("export metadata sanitization and COM-less normalization", [&] {
			auto authored = fixture;
			// NAF's mismatched accessor extrema must not prevent otherwise valid imports.
			authored.doc["accessors"][1]["min"] = { 0 };
			authored.doc["accessors"][1]["max"] = { 2 };
			VerifyPrimary(GLTFImport::LoadAnimation("export-metadata", authored.Glb(), temp.path, ""));
			authored.doc["nodes"][1]["name"] = "ArmatureEXP";
			const auto normalized = GLTFImport::LoadAnimation("comless", authored.Glb(), temp.path, "");
			if (Success(normalized)) {
				Near(Sample(normalized, 0.5f)[2][13], 0, "large unkeyed COM-less body root loses authoring height");
				Near(Sample(normalized, 0.5f)[3][14], 0.25f, "ordinary child rest translation remains unchanged");
			}
		});
		Run("missing, truncated and malformed content", [&] {
			Failure(GLTFImport::LoadAnimation(temp.path / "missing.glb", ""), GLTFError::kFileReadFailed);
			Failure(GLTFImport::LoadAnimation("empty", {}, temp.path, ""), GLTFError::kFileReadFailed);
			Failure(GLTFImport::LoadAnimation("bad-json", TextBytes("{not-json}"), temp.path, ""), GLTFError::kFileReadFailed);
			Failure(GLTFImport::LoadAnimation("truncated-glb", Bytes(glb.begin(), glb.begin() + 24), temp.path, ""), GLTFError::kFileReadFailed);
			auto gzip = Gzip(glb);
			gzip.resize(gzip.size() / 2);
			Failure(GLTFImport::LoadAnimation("truncated-gzip", gzip, temp.path, ""), GLTFError::kFileReadFailed);
			auto invalid = fixture;
			invalid.doc["asset"]["version"] = "1.0";
			Failure(GLTFImport::LoadAnimation("unsupported-version", invalid.Glb(), temp.path, ""), GLTFError::kParseFailed);
			invalid = fixture;
			invalid.doc.erase("animations");
			Failure(GLTFImport::LoadAnimation("no-animation", invalid.Glb(), temp.path, ""), GLTFError::kNoAnimations);
			invalid = fixture;
			invalid.doc["nodes"][3]["children"] = { 0 };
			Failure(GLTFImport::LoadAnimation("cyclic-skeleton", invalid.Glb(), temp.path, ""), GLTFError::kFailedToBuildSkeleton);
		});
		Run("out-of-bounds channel falls back to bind pose", [&] {
			auto invalid = fixture;
			// Too short for two VEC3 keys: skip the channel before fastgltf reads past the view.
			invalid.doc["bufferViews"][1]["byteLength"] = 12;
			const auto result = GLTFImport::LoadAnimation("short-accessor", invalid.Glb(), temp.path, "");
			if (Success(result)) {
				const auto pose = Sample(result, 0.5f);
				Near(pose[1][12], 0, "invalid translation channel falls back to rest X");
				Near(pose[1][13], 0, "invalid translation channel falls back to rest Y");
				Near(pose[1][14], 0, "invalid translation channel falls back to rest Z");
				Near(pose[1][0], 2, "valid scale channel survives rejected translation");
			}
		});
	} catch (const std::exception& error) {
		Check(false, std::string("fixture setup failed: ") + error.what());
	}
	return OSF::Test::Finish("GLTF import smoke");
}
