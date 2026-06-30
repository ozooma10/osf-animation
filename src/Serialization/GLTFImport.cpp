// GLTF -> ozz-animation import. Reduced to bones only, with the skeleton derived from the
// GLTF node hierarchy. Adapted from NativeAnimationFrameworkSF (Serialization/GLTFImport.cpp).
// Copyright (C) Deweh, https://github.com/Deweh/NativeAnimationFrameworkSF

#include "GLTFImport.h"

#include "Util/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/maths/transform.h"

template <>
struct fastgltf::ElementTraits<ozz::math::Quaternion> : fastgltf::ElementTraitsBase<ozz::math::Quaternion, AccessorType::Vec4, float>
{};

template <>
struct fastgltf::ElementTraits<ozz::math::Float3> : fastgltf::ElementTraitsBase<ozz::math::Float3, AccessorType::Vec3, float>
{};

namespace OSF::Serialization
{
	namespace
	{
		using OSF::Util::ToLower;

		// glTF (NAF/SAF) clips are authored in METERS; the game rig is in game units (Bethesda
		// 1 unit = 0.0142875 m). Without this scale the whole skeleton imports ~70x too small, the
		// bones collapse onto the root and the skinned mesh fans out from a point ("fly away really
		// far"). .af content is native game units via AFImport and is unaffected by this path.
		constexpr float kUnitsPerMeter = 1.0f / 0.0142875f;  // ~69.996

		// NAF/SAF clips bake the actor's authoring-space placement into the export-root nodes
		// ("HumanExportRoot"/"Root"). OSF positions the actor via the scene anchor + compose-root pin,
		// so applying the clip root too would double-position and shove the body off the anchor. Drop
		// the root TRANSLATION (rotation is kept) — this is what SAF does (its log: skippedRoot=2);
		// COM and the bones below it carry the actual pose/offset.
		bool IsExportRootNode(std::string_view a_lowerName)
		{
			return a_lowerName == "humanexportroot" || a_lowerName == "root";
		}

		ozz::math::Transform GetNodeLocalTransform(const fastgltf::Node& a_node)
		{
			ozz::math::Transform result = ozz::math::Transform::identity();
			if (std::holds_alternative<fastgltf::TRS>(a_node.transform)) {
				const auto& trs = std::get<fastgltf::TRS>(a_node.transform);
				result.rotation = { trs.rotation[0], trs.rotation[1], trs.rotation[2], trs.rotation[3] };
				result.scale = { trs.scale[0], trs.scale[1], trs.scale[2] };
				if (IsExportRootNode(ToLower(a_node.name.c_str()))) {
					result.translation = { 0.0f, 0.0f, 0.0f };  // anchor owns root placement
				} else {
					result.translation = { trs.translation[0] * kUnitsPerMeter, trs.translation[1] * kUnitsPerMeter, trs.translation[2] * kUnitsPerMeter };
				}
			}
			return result;
		}

		// If the buffer is gzip-compressed, decompresses it transparently.
		std::optional<std::vector<std::byte>> MaybeGunzip(std::vector<std::byte> a_buffer)
		{
			const bool isGzip = a_buffer.size() >= 2 &&
				a_buffer[0] == std::byte{ 0x1F } && a_buffer[1] == std::byte{ 0x8B };
			if (!isGzip) {
				return a_buffer;
			}

			z_stream strm{};
			// 15 = max window, +16 = expect a gzip header
			if (inflateInit2(&strm, 15 + 16) != Z_OK) {
				return std::nullopt;
			}

			std::vector<std::byte> out;
			std::byte chunk[65536];
			strm.next_in = reinterpret_cast<Bytef*>(a_buffer.data());
			strm.avail_in = static_cast<uInt>(a_buffer.size());

			int ret = Z_OK;
			do {
				strm.next_out = reinterpret_cast<Bytef*>(chunk);
				strm.avail_out = sizeof(chunk);
				ret = inflate(&strm, Z_NO_FLUSH);
				if (ret != Z_OK && ret != Z_STREAM_END) {
					inflateEnd(&strm);
					return std::nullopt;
				}
				out.insert(out.end(), chunk, chunk + (sizeof(chunk) - strm.avail_out));
			} while (ret != Z_STREAM_END);

			inflateEnd(&strm);
			return out;
		}

		// Reads the whole file; if it is gzip-compressed, decompresses it transparently.
		std::optional<std::vector<std::byte>> ReadFileMaybeGzip(const std::filesystem::path& a_file)
		{
			std::error_code ec;
			const auto fileSize = std::filesystem::file_size(a_file, ec);
			if (ec) {
				return std::nullopt;
			}

			std::ifstream file(a_file, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}

			std::vector<std::byte> buffer(fileSize);
			if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize))) {
				return std::nullopt;
			}

			return MaybeGunzip(std::move(buffer));
		}

		// Strips `min`/`max` from all accessors in a glTF JSON document. NAF's exporter writes min/max arrays whose length doesn't match the accessor type (spec violation), which strict parsers reject. 
		// We never use min/max, so dropping them is safe.
		bool SanitizeGltfJson(std::string_view a_json, std::string& a_out)
		{
			auto doc = nlohmann::json::parse(a_json, nullptr, false);
			if (doc.is_discarded()) {
				return false;
			}

			if (auto it = doc.find("accessors"); it != doc.end() && it->is_array()) {
				for (auto& accessor : *it) {
					accessor.erase("min");
					accessor.erase("max");
				}
			}

			a_out = doc.dump();
			return true;
		}

		// Rewrites a GLB/glTF byte buffer with sanitized JSON. GLB layout:
		// 12-byte header, then chunks of [u32 length][u32 type][data]; the first
		// chunk is JSON, everything after it is copied verbatim.
		bool SanitizeGltfBytes(std::vector<std::byte>& a_bytes)
		{
			constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
			constexpr uint32_t kJsonChunk = 0x4E4F534A; // "JSON"

			const auto readU32 = [&](size_t a_offset) {
				uint32_t v;
				std::memcpy(&v, a_bytes.data() + a_offset, 4);
				return v;
			};

			if (a_bytes.size() >= 2 && static_cast<char>(a_bytes[0]) == '{') {
				// plain .gltf JSON
				std::string sanitized;
				if (!SanitizeGltfJson({ reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size() }, sanitized)) {
					return false;
				}
				a_bytes.assign(reinterpret_cast<const std::byte*>(sanitized.data()),
					reinterpret_cast<const std::byte*>(sanitized.data() + sanitized.size()));
				return true;
			}

			if (a_bytes.size() < 20 || readU32(0) != kGlbMagic || readU32(12 + 4) != kJsonChunk) {
				return false;
			}

			const uint32_t jsonLen = readU32(12);
			if (20 + static_cast<size_t>(jsonLen) > a_bytes.size()) {
				return false;
			}

			std::string sanitized;
			if (!SanitizeGltfJson({ reinterpret_cast<const char*>(a_bytes.data() + 20), jsonLen }, sanitized)) {
				return false;
			}
			sanitized.resize((sanitized.size() + 3) & ~size_t{ 3 }, ' ');  // 4-byte pad with spaces

			std::vector<std::byte> out;
			out.reserve(a_bytes.size());
			const auto append = [&](const void* a_data, size_t a_size) {
				const auto* p = static_cast<const std::byte*>(a_data);
				out.insert(out.end(), p, p + a_size);
			};

			append(a_bytes.data(), 12);                       // header (total length fixed below)
			const uint32_t newJsonLen = static_cast<uint32_t>(sanitized.size());
			append(&newJsonLen, 4);
			append(a_bytes.data() + 16, 4);                   // "JSON" chunk type
			append(sanitized.data(), sanitized.size());
			append(a_bytes.data() + 20 + jsonLen, a_bytes.size() - 20 - jsonLen);  // remaining chunks

			const uint32_t totalLen = static_cast<uint32_t>(out.size());
			std::memcpy(out.data() + 8, &totalLen, 4);

			a_bytes = std::move(out);
			return true;
		}

		// Sets a_error + a_detail and returns nullopt on failure. The read/sanitize stages report
		// kFileReadFailed; the fastgltf stages report kParseFailed. (Previously the caller recovered
		// the category by sniffing a_detail's prefix — control flow keyed on log prose.)
		std::optional<fastgltf::Asset> LoadAssetFromBytes(std::vector<std::byte> a_bytes,
			const std::filesystem::path& a_parentPath, GLTFError& a_error, std::string& a_detail)
		{
			auto bytes = MaybeGunzip(std::move(a_bytes));
			if (!bytes) {
				a_error = GLTFError::kFileReadFailed;
				a_detail = "gzip decompression failed";
				return std::nullopt;
			}

			if (!SanitizeGltfBytes(*bytes)) {
				a_error = GLTFError::kFileReadFailed;
				a_detail = "not a parseable glTF/GLB file";
				return std::nullopt;
			}

			auto data = fastgltf::GltfDataBuffer::FromBytes(bytes->data(), bytes->size());
			if (data.error() != fastgltf::Error::None) {
				a_error = GLTFError::kParseFailed;
				a_detail = std::format("fastgltf buffer error: {}", fastgltf::getErrorMessage(data.error()));
				return std::nullopt;
			}

			fastgltf::Parser parser;
			auto gltfOptions =
				fastgltf::Options::LoadExternalBuffers |
				fastgltf::Options::DecomposeNodeMatrices;

			auto gltfCategories =
				fastgltf::Category::Animations |
				fastgltf::Category::Nodes |
				fastgltf::Category::Buffers |
				fastgltf::Category::BufferViews |
				fastgltf::Category::Accessors;

			auto gltf = parser.loadGltf(data.get(), a_parentPath, gltfOptions, gltfCategories);
			if (gltf.error() != fastgltf::Error::None) {
				a_error = GLTFError::kParseFailed;
				a_detail = std::format("fastgltf parse error: {}", fastgltf::getErrorMessage(gltf.error()));
				return std::nullopt;
			}

			return std::move(gltf.get());
		}

		std::optional<fastgltf::Asset> LoadAsset(const std::filesystem::path& a_file, GLTFError& a_error, std::string& a_detail)
		{
			auto bytes = ReadFileMaybeGzip(a_file);
			if (!bytes) {
				a_error = GLTFError::kFileReadFailed;
				a_detail = "file missing, unreadable or gzip decompression failed";
				return std::nullopt;
			}
			return LoadAssetFromBytes(std::move(*bytes), a_file.parent_path(), a_error, a_detail);
		}
		// Builds an ozz skeleton from the asset's full node hierarchy. Root nodes
		// are nodes that are not referenced as a child of any other node, so the
		// Scenes category is not required.
		std::shared_ptr<Animation::OzzSkeleton> BuildSkeleton(const fastgltf::Asset& a_asset)
		{
			const auto& nodes = a_asset.nodes;
			std::vector<bool> isChild(nodes.size(), false);
			for (const auto& n : nodes) {
				for (auto childIdx : n.children) {
					if (childIdx < nodes.size()) {
						isChild[childIdx] = true;
					}
				}
			}

			using RawSkeleton = ozz::animation::offline::RawSkeleton;
			RawSkeleton raw;

			auto addJoint = [&](this auto&& self, size_t a_nodeIdx, RawSkeleton::Joint::Children& a_dest) -> void {
				const auto& n = nodes[a_nodeIdx];
				auto& joint = a_dest.emplace_back();
				joint.name = n.name.c_str();
				joint.transform = GetNodeLocalTransform(n);
				for (auto childIdx : n.children) {
					if (childIdx < nodes.size()) {
						self(childIdx, joint.children);
					}
				}
			};

			for (size_t i = 0; i < nodes.size(); i++) {
				if (!isChild[i]) {
					addJoint(i, raw.roots);
				}
			}

			if (!raw.Validate()) {
				return nullptr;
			}

			ozz::animation::offline::SkeletonBuilder builder;
			auto skeleton = builder(raw);
			if (!skeleton) {
				return nullptr;
			}

			auto result = std::make_shared<Animation::OzzSkeleton>();
			result->data = std::move(skeleton);
			return result;
		}

		std::shared_ptr<Animation::OzzAnimation> BuildAnimation(const fastgltf::Asset& a_asset,
			const fastgltf::Animation& a_anim, const ozz::animation::Skeleton& a_skeleton)
		{
			// Map of lowercased node names -> skeleton joint indexes.
			std::map<std::string, size_t> jointMap;
			auto jointNames = a_skeleton.joint_names();
			for (size_t i = 0; i < jointNames.size(); i++) {
				jointMap[ToLower(jointNames[i])] = i;
			}

			// Map of GLTF node indexes -> skeleton joint indexes.
			std::vector<size_t> nodeToJoint;
			nodeToJoint.reserve(a_asset.nodes.size());
			for (const auto& n : a_asset.nodes) {
				if (auto iter = jointMap.find(ToLower(n.name.c_str())); iter != jointMap.end()) {
					nodeToJoint.push_back(iter->second);
				} else {
					nodeToJoint.push_back(SIZE_MAX);
				}
			}

			const int numJoints = a_skeleton.num_joints();
			auto raw = std::make_unique<ozz::animation::offline::RawAnimation>();
			raw->duration = 0.001f;
			raw->tracks.resize(numJoints);

			for (const auto& c : a_anim.channels) {
				if (!c.nodeIndex.has_value() || c.nodeIndex.value() >= a_asset.nodes.size())
					continue;

				if (c.path != fastgltf::AnimationPath::Rotation &&
					c.path != fastgltf::AnimationPath::Translation &&
					c.path != fastgltf::AnimationPath::Scale)
					continue;

				auto jointIdx = nodeToJoint[c.nodeIndex.value()];
				if (jointIdx == SIZE_MAX)
					continue;

				// Export-root translation is dropped (placement is anchor-owned); rotation still applies.
				const bool isExportRoot = IsExportRootNode(ToLower(a_asset.nodes[c.nodeIndex.value()].name.c_str()));

				if (c.samplerIndex >= a_anim.samplers.size())
					continue;

				const auto& sampler = a_anim.samplers[c.samplerIndex];
				if (sampler.inputAccessor >= a_asset.accessors.size() ||
					sampler.outputAccessor >= a_asset.accessors.size())
					continue;

				const auto& timeAccessor = a_asset.accessors[sampler.inputAccessor];
				const auto& dataAccessor = a_asset.accessors[sampler.outputAccessor];
				if (timeAccessor.count != dataAccessor.count)
					continue;

				auto& track = raw->tracks[jointIdx];
				for (size_t i = 0; i < timeAccessor.count; i++) {
					float t = fastgltf::getAccessorElement<float>(a_asset, timeAccessor, i);
					if (t > raw->duration)
						raw->duration = t;

					switch (c.path) {
					case fastgltf::AnimationPath::Rotation:
						track.rotations.push_back({ t, fastgltf::getAccessorElement<ozz::math::Quaternion>(a_asset, dataAccessor, i) });
						break;
					case fastgltf::AnimationPath::Translation: {
						const auto tRaw = fastgltf::getAccessorElement<ozz::math::Float3>(a_asset, dataAccessor, i);
						const ozz::math::Float3 tr = isExportRoot ?
							ozz::math::Float3(0.0f, 0.0f, 0.0f) :
							ozz::math::Float3(tRaw.x * kUnitsPerMeter, tRaw.y * kUnitsPerMeter, tRaw.z * kUnitsPerMeter);
						track.translations.push_back({ t, tr });
						break;
					}
					case fastgltf::AnimationPath::Scale:
						track.scales.push_back({ t, fastgltf::getAccessorElement<ozz::math::Float3>(a_asset, dataAccessor, i) });
						break;
					default:
						break;
					}
				}
			}

			// Any joint without keyframes holds its skeleton rest pose.
			for (int i = 0; i < numJoints; i++) {
				auto& track = raw->tracks[i];
				const auto rest = ozz::animation::GetJointLocalRestPose(a_skeleton, i);
				if (track.rotations.empty()) {
					track.rotations.push_back({ 0.0001f, rest.rotation });
				}
				if (track.translations.empty()) {
					track.translations.push_back({ 0.0001f, rest.translation });
				}
				if (track.scales.empty()) {
					track.scales.push_back({ 0.0001f, rest.scale });
				}
			}

			ozz::animation::offline::AnimationBuilder builder;
			auto anim = builder(*raw);
			if (!anim) {
				return nullptr;
			}

			auto result = std::make_shared<Animation::OzzAnimation>();
			result->data = std::move(anim);
			return result;
		}
	}

	LoadResult BuildLoadResult(fastgltf::Asset& a_asset, std::string_view a_animId)
	{
		LoadResult result;

		if (a_asset.animations.empty()) {
			result.error = GLTFError::kNoAnimations;
			result.detail = "file contains no animations";
			return result;
		}

		const fastgltf::Animation* anim = nullptr;
		if (a_animId.empty()) {
			anim = &a_asset.animations[0];
		} else if (std::all_of(a_animId.begin(), a_animId.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
			// from_chars (not stoull): an over-long all-digit id (> ULLONG_MAX)
			// makes stoull THROW std::out_of_range, which would propagate out of
			// the Papyrus native and crash the game. from_chars reports overflow
			// as result_out_of_range, leaving anim null -> kInvalidAnimationIdentifier.
			size_t idx = 0;
			const auto* first = a_animId.data();
			const auto* last = first + a_animId.size();
			if (std::from_chars(first, last, idx).ec == std::errc{} && idx < a_asset.animations.size()) {
				anim = &a_asset.animations[idx];
			}
		} else {
			for (const auto& a : a_asset.animations) {
				if (std::string_view{ a.name } == a_animId) {
					anim = &a;
					break;
				}
			}
		}

		if (!anim) {
			result.error = GLTFError::kInvalidAnimationIdentifier;
			result.detail = std::format("no animation '{}' in file ({} animations present)", a_animId, a_asset.animations.size());
			return result;
		}

		auto skeleton = BuildSkeleton(a_asset);
		if (!skeleton) {
			result.error = GLTFError::kFailedToBuildSkeleton;
			result.detail = std::format("ozz skeleton build failed ({} nodes)", a_asset.nodes.size());
			return result;
		}

		auto runtimeAnim = BuildAnimation(a_asset, *anim, *skeleton->data);
		if (!runtimeAnim) {
			result.error = GLTFError::kFailedToMakeClip;
			result.detail = "ozz animation build failed";
			return result;
		}

		result.skeleton = std::move(skeleton);
		result.anim = std::move(runtimeAnim);
		result.error = GLTFError::kSuccess;
		return result;
		}

	namespace
	{
		// Clip cache (successes only). Note: no logging in this file — it also
		// builds into the standalone import-test tool, which doesn't link the
		// game libraries. Deliberately leaked: destroying the map at process exit
		// would free ozz objects after ozz's own statics are gone, which crashed
		// the import tool after main with STATUS_STACK_BUFFER_OVERRUN.
		std::mutex g_clipCacheLock;
		auto& ClipCache()
		{
			static auto* cache = new std::unordered_map<std::string, LoadResult>();
			return *cache;
		}

		std::string ClipCacheKey(std::string_view a_cacheKey, std::string_view a_animId)
		{
			return ToLower(std::string(a_cacheKey)) + '|' + std::string(a_animId);
		}

		std::string ClipCacheKey(const std::filesystem::path& a_file, std::string_view a_animId)
		{
			std::error_code ec;
			auto norm = std::filesystem::weakly_canonical(a_file, ec);
			if (ec) {
				norm = a_file.lexically_normal();
			}
			auto normString = norm.string();
			return ClipCacheKey(std::string_view{ normString }, a_animId);
		}

	}

	LoadResult GLTFImport::LoadAnimation(const std::filesystem::path& a_file, std::string_view a_animId)
	{
		const auto key = ClipCacheKey(a_file, a_animId);
		{
			std::scoped_lock l{ g_clipCacheLock };
			auto& cache = ClipCache();
			if (auto it = cache.find(key); it != cache.end()) {
				return it->second;
			}
		}

		auto result = LoadAnimationUncached(a_file, a_animId);
		if (result.error == GLTFError::kSuccess) {
			// Two threads importing the same file concurrently both pay the
			// import and one insert wins — benign (identical immutable data).
			std::scoped_lock l{ g_clipCacheLock };
			ClipCache().emplace(key, result);
		}
		return result;
	}

	LoadResult GLTFImport::LoadAnimation(std::string_view a_cacheKey, std::vector<std::byte> a_bytes,
		const std::filesystem::path& a_parentPath, std::string_view a_animId)
	{
		const auto key = ClipCacheKey(a_cacheKey, a_animId);
		{
			std::scoped_lock l{ g_clipCacheLock };
			auto& cache = ClipCache();
			if (auto it = cache.find(key); it != cache.end()) {
				return it->second;
			}
		}

		auto result = LoadAnimationUncached(std::move(a_bytes), a_parentPath, a_animId);
		if (result.error == GLTFError::kSuccess) {
			std::scoped_lock l{ g_clipCacheLock };
			ClipCache().emplace(key, result);
		}
		return result;
	}

	void GLTFImport::ClearCache()
	{
		std::scoped_lock l{ g_clipCacheLock };
		ClipCache().clear();
	}

	LoadResult GLTFImport::LoadAnimationUncached(const std::filesystem::path& a_file, std::string_view a_animId)
	{
		LoadResult result;

		GLTFError loadError = GLTFError::kFileReadFailed;
		std::string detail;
		auto asset = LoadAsset(a_file, loadError, detail);
		if (!asset) {
			result.error = loadError;
			result.detail = std::move(detail);
			return result;
		}

		return BuildLoadResult(*asset, a_animId);
	}

	LoadResult GLTFImport::LoadAnimationUncached(std::vector<std::byte> a_bytes,
		const std::filesystem::path& a_parentPath, std::string_view a_animId)
	{
		LoadResult result;

		GLTFError loadError = GLTFError::kFileReadFailed;
		std::string detail;
		auto asset = LoadAssetFromBytes(std::move(a_bytes), a_parentPath, loadError, detail);
		if (!asset) {
			result.error = loadError;
			result.detail = std::move(detail);
			return result;
		}

		return BuildLoadResult(*asset, a_animId);
	}
}
