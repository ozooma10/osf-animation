#include "Serialization/ClipDurationProbe.h"
#include "Serialization/AFImport.h"
#include "Util/Ba2.h"
#include "Util/ClipPath.h"
#include "Util/Gzip.h"
#include "Util/StringUtil.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace OSF::Serialization::ClipDurationProbe
{
	namespace
	{
		using json = nlohmann::json;
		// .af header: u16 frameCount @44 (see AFImport::ParseAf); duration = (frameCount - 1) / kAfFps.
		std::optional<float> ProbeAfBytes(const std::vector<std::byte>& a_bytes)
		{
			if (a_bytes.size() < 64) {
				return std::nullopt;
			}
			std::uint16_t frameCount = 0;
			std::memcpy(&frameCount, a_bytes.data() + 44, sizeof(frameCount));
			const float lastFrame = frameCount > 1 ? static_cast<float>(frameCount - 1) : 1.0f;
			return lastFrame / AFImport::kAfFps;
		}

		// Must mirror GLTFImport::BuildLoadResult's selection: "" = first, digits = index, else name.
		const json* SelectAnimation(const json& a_animations, std::string_view a_animId)
		{
			if (!a_animations.is_array() || a_animations.empty()) {
				return nullptr;
			}
			if (a_animId.empty()) {
				return &a_animations[0];
			}
			if (std::all_of(a_animId.begin(), a_animId.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
				std::size_t idx = 0;
				const auto* first = a_animId.data();
				if (std::from_chars(first, first + a_animId.size(), idx).ec == std::errc{} && idx < a_animations.size()) {
					return &a_animations[idx];
				}
				return nullptr;
			}
			for (const auto& a : a_animations) {
				if (a.value("name", std::string{}) == a_animId) {
					return &a;
				}
			}
			return nullptr;
		}

		// Duration = last element of each sampler's input accessor (times are strictly increasing).
		// Accessor `max` is only a fallback: NAF exports carry bogus min/max (see SanitizeGltfJson).
		std::optional<float> ProbeGltfBytes(std::vector<std::byte> a_bytes, std::string_view a_animId)
		{
			auto bytes = Util::MaybeGunzip(std::move(a_bytes));
			if (!bytes || bytes->size() < 4) {
				return std::nullopt;
			}

			std::string_view jsonText;
			const std::byte* bin = nullptr;
			std::size_t      binSize = 0;

			const auto readU32 = [&](std::size_t a_off) {
				std::uint32_t v = 0;
				std::memcpy(&v, bytes->data() + a_off, 4);
				return v;
			};
			constexpr std::uint32_t kGlbMagic = 0x46546C67;   // "glTF"
			constexpr std::uint32_t kJsonChunk = 0x4E4F534A;  // "JSON"
			constexpr std::uint32_t kBinChunk = 0x004E4942;   // "BIN\0"

			if (readU32(0) == kGlbMagic) {
				std::size_t off = 12;
				while (off + 8 <= bytes->size()) {
					const std::uint32_t len = readU32(off);
					const std::uint32_t type = readU32(off + 4);
					if (off + 8 + len > bytes->size()) {
						break;
					}
					if (type == kJsonChunk && jsonText.empty()) {
						jsonText = { reinterpret_cast<const char*>(bytes->data() + off + 8), len };
					} else if (type == kBinChunk && !bin) {
						bin = bytes->data() + off + 8;
						binSize = len;
					}
					off += 8 + static_cast<std::size_t>(len);
				}
			} else if (static_cast<char>((*bytes)[0]) == '{') {
				jsonText = { reinterpret_cast<const char*>(bytes->data()), bytes->size() };
			}
			if (jsonText.empty()) {
				return std::nullopt;
			}

			const json doc = json::parse(jsonText, nullptr, /*allow_exceptions*/ false);
			if (!doc.is_object()) {
				return std::nullopt;
			}
			const auto animsIt = doc.find("animations");
			if (animsIt == doc.end()) {
				return std::nullopt;
			}
			const json* anim = SelectAnimation(*animsIt, a_animId);
			if (!anim) {
				return std::nullopt;
			}
			const auto accessors = doc.find("accessors");
			const auto bufferViews = doc.find("bufferViews");

			const auto lastInputKey = [&](const json& a_acc) -> std::optional<float> {
				if (!bin || bufferViews == doc.end() || !bufferViews->is_array()) {
					return std::nullopt;
				}
				if (a_acc.value("componentType", 0) != 5126 /*FLOAT*/ || a_acc.value("type", std::string{}) != "SCALAR" || a_acc.contains("sparse")) {
					return std::nullopt;
				}
				const std::size_t count = a_acc.value("count", std::size_t{ 0 });
				const std::size_t bvIdx = a_acc.value("bufferView", std::size_t{ SIZE_MAX });
				if (count == 0 || bvIdx >= bufferViews->size()) {
					return std::nullopt;
				}
				const json& bv = (*bufferViews)[bvIdx];
				if (bv.value("buffer", std::size_t{ 0 }) != 0) {
					return std::nullopt;  // GLB binary chunk is always buffer 0
				}
				const std::size_t stride = bv.value("byteStride", std::size_t{ 4 });
				const std::size_t bvOff = bv.value("byteOffset", std::size_t{ 0 });
				const std::size_t accOff = a_acc.value("byteOffset", std::size_t{ 0 });
				// Bounds checked stepwise: all terms are file-declared and a wrapped sum would read out of bounds.
				if (stride < 4 || bvOff > binSize || accOff > binSize - bvOff) {
					return std::nullopt;
				}
				const std::size_t base = bvOff + accOff;
				const std::size_t avail = binSize - base;
				if (avail < 4 || count - 1 > (avail - 4) / stride) {
					return std::nullopt;
				}
				const std::size_t off = base + (count - 1) * stride;
				float             t = 0.0f;
				std::memcpy(&t, bin + off, 4);
				return t;
			};

			float      maxTime = -1.0f;
			const auto samplers = anim->find("samplers");
			if (samplers == anim->end() || !samplers->is_array()) {
				return std::nullopt;
			}
			for (const auto& s : *samplers) {
				const std::size_t inIdx = s.value("input", std::size_t{ SIZE_MAX });
				if (accessors == doc.end() || !accessors->is_array() || inIdx >= accessors->size()) {
					continue;
				}
				const json&          acc = (*accessors)[inIdx];
				std::optional<float> t = lastInputKey(acc);
				if (!t) {
					if (const auto mx = acc.find("max"); mx != acc.end() && mx->is_array() && !mx->empty() && (*mx)[0].is_number()) {
						t = (*mx)[0].get<float>();
					}
				}
				if (t && std::isfinite(*t) && *t >= 0.0f) {
					maxTime = std::max(maxTime, *t);
				}
			}
			if (maxTime < 0.0f) {
				return std::nullopt;
			}
			return std::max(maxTime, 0.001f);  // matches the importer's duration floor
		}

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path& a_file)
		{
			std::error_code ec;
			const auto      sz = std::filesystem::file_size(a_file, ec);
			if (ec || sz > Util::kMaxClipBytes) {
				return std::nullopt;
			}
			std::ifstream          f(a_file, std::ios::binary);
			std::vector<std::byte> buf(sz);
			if (!f || (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz)))) {
				return std::nullopt;
			}
			return buf;
		}

	}
	API::ClipProbeResult Probe(std::string_view file, std::string_view animId, const API::ClipProbeResult* previous)
	{
		API::ClipProbeResult result;
		const std::string    a_fileSpec{ file };
		const auto           spec = Util::ResolveClipSpec(std::filesystem::path{ a_fileSpec });
		const bool           isAf = Util::ToLower(std::filesystem::path{ a_fileSpec }.extension().string()) == ".af";

		const auto cacheFresh = [&](std::uint64_t size, std::int64_t modified) { return previous && previous->bytes == size && previous->modified == modified; };

		// Loose files first, then game BA2s for .af only. GLBs in mod BA2s are backfilled by Record
		// (ReadGameFile rescans every archive per call, too slow for a bulk pass).
		std::vector<std::byte> bytes;
		std::uint64_t          size = 0;
		std::int64_t           mtime = 0;
		bool                   found = false;

		for (const auto& cand : spec.candidates) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(cand.filePath, ec)) {
				continue;
			}
			size = std::filesystem::file_size(cand.filePath, ec);
			mtime = ec ? 0 : std::filesystem::last_write_time(cand.filePath, ec).time_since_epoch().count();
			if (cacheFresh(size, mtime)) {
				result.status = API::ClipProbeStatus::Unchanged;
				return result;
			}
			if (auto b = ReadWholeFile(cand.filePath)) {
				bytes = std::move(*b);
				found = true;
			}
			break;
		}

		if (!found && isAf) {
			for (const auto& cand : spec.candidates) {
				if (!cand.resource) {
					continue;
				}
				if (auto b = Util::Ba2::ReadGameFile(cand.resourcePath, "Starfield - Animations.ba2")) {
					bytes.resize(b->size());
					if (!b->empty()) {
						std::memcpy(bytes.data(), b->data(), b->size());
					}
					size = bytes.size();
					mtime = 0;
					found = true;
					break;
				}
			}
			if (found && cacheFresh(size, 0)) {
				result.status = API::ClipProbeStatus::Unchanged;
				return result;
			}
		}

		if (!found)
			return result;
		result.bytes = size;
		result.modified = mtime;
		const auto seconds = isAf ? ProbeAfBytes(bytes) : ProbeGltfBytes(std::move(bytes), animId);
		if (seconds && std::isfinite(*seconds) && *seconds > 0 && *seconds <= 3600) {
			result.seconds = *seconds;
			result.status = API::ClipProbeStatus::Available;
		} else
			result.status = API::ClipProbeStatus::Unprobeable;
		return result;
	}
}
