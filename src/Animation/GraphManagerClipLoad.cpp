#include "Animation/GraphManagerClipLoad.h"

#include "Animation/GraphManager.h"
#include "Serialization/AFImport.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/GLTFImport.h"
#include "Util/ClipPath.h"
#include "Util/Profile.h"
#include "Util/Species.h"
#include "Util/StringUtil.h"

#include "RE/S/StreamBase.h"

#include <cstring>
#include <format>
#include <mutex>
#include <unordered_set>

namespace OSF::Animation
{
	namespace
	{
		void WarnImplicitClipFallback(std::string_view a_spec, std::string_view a_resolved)
		{
			// A pack can reuse one clip across many scenes/stages. Keep this migration warning useful
			// without flooding the shipped Info-level log on every playback.
			static std::mutex warnedLock;
			static std::unordered_set<std::string> warnedSpecs;
			bool first = false;
			{
				std::scoped_lock l{ warnedLock };
				first = warnedSpecs.emplace(a_spec).second;
			}
			if (first) {
				REX::WARN("[Anim] clip '{}' resolved through deprecated implicit fallback '{}' — use an explicit Data-relative path or naf: spec",
					a_spec, a_resolved);
			}
		}

		class ResourceBinaryStream
		{
		public:
			ResourceBinaryStream() = delete;

			explicit ResourceBinaryStream(const char* a_path)
			{
				using func_t = void (*)(ResourceBinaryStream*, const char*);
				static REL::Relocation<func_t> func{ REL::ID(147134) };  // BSResourceNiBinaryStream ctor
				func(this, a_path);
			}

			ResourceBinaryStream(const ResourceBinaryStream&) = delete;
			ResourceBinaryStream& operator=(const ResourceBinaryStream&) = delete;

			virtual ~ResourceBinaryStream()
			{
				using func_t = void (*)(ResourceBinaryStream*);
				static REL::Relocation<func_t> func{ REL::ID(147137) };  // BSResourceNiBinaryStream dtor
				func(this);
			}

			[[nodiscard]] bool good() const noexcept { return stream != nullptr; }
			[[nodiscard]] std::uint32_t size() const noexcept { return stream ? stream->totalSize : 0; }

			std::uint64_t read(void* a_buffer, std::uint64_t a_bytes)
			{
				using func_t = std::uint64_t (*)(ResourceBinaryStream*, void*, std::uint64_t);
				static REL::Relocation<func_t> func{ REL::ID(147139) };  // BSResourceNiBinaryStream::DoRead
				return func(this, a_buffer, a_bytes);
			}

			std::uint64_t                absolutePos = 0;  // NiBinaryStream base
			RE::BSResource::StreamBase* stream = nullptr;
			void*                        buffer = nullptr;
			std::uint64_t                streamPos = 0;
			RE::BSResource::ErrorCode    lastError = RE::BSResource::ErrorCode::kNone;
		};
		static_assert(sizeof(ResourceBinaryStream) == 0x30);

		std::optional<std::vector<std::uint8_t>> ReadResourceFile(std::string_view a_relPath)
		{
			const auto relPath = Util::NormalizeResourcePath(a_relPath);
			if (relPath.empty()) {
				return std::nullopt;
			}

			ResourceBinaryStream stream{ relPath.c_str() };
			if (!stream.good()) {
				return std::nullopt;
			}

			std::vector<std::uint8_t> bytes(stream.size());
			if (!bytes.empty()) {
				const auto read = stream.read(bytes.data(), bytes.size());
				if (read != bytes.size()) {
					return std::nullopt;
				}
			}
			return bytes;
		}

		// Raw bytes of the human skeleton.rig for the .af importer. BSResource preserves the
		// engine's loose-file-over-archive precedence, so an extracted rig still overrides BA2.
		std::optional<std::vector<std::uint8_t>> LoadHumanRigBytes()
		{
			const char* candidates[] = {
				"OSF\\skeleton.rig",
				"OSF\\Animations\\skeleton.rig",
				"meshes\\actors\\human\\characterassets\\skeleton.rig",
			};
			for (const auto* rel : candidates) {
				if (auto bytes = ReadResourceFile(rel); bytes && !bytes->empty()) {
					REX::TRACE("[Anim] rig: resource {} ({} bytes)", rel, bytes->size());
					return bytes;
				}
			}
			REX::ERROR("[Anim] rig: skeleton.rig unavailable through BSResource");
			return std::nullopt;
		}

		// A non-human species' skeleton.rig, at a_rigResPath (derived from the clip's own path by
		// Util::RigResourcePathFromAnimPath). Falls back to the human rig if the species rig can't
		// be read, so an unexpected path degrades to a (wrong-but-safe) bind instead of no clip.
		std::optional<std::vector<std::uint8_t>> LoadSpeciesRigBytes(const std::string& a_rigResPath)
		{
			if (auto bytes = ReadResourceFile(a_rigResPath); bytes && !bytes->empty()) {
				REX::TRACE("[Anim] rig: species resource {} ({} bytes)", a_rigResPath, bytes->size());
				return bytes;
			}
			REX::WARN("[Anim] rig: species rig '{}' unavailable — falling back to human", a_rigResPath);
			return LoadHumanRigBytes();
		}

		std::vector<std::byte> ToByteVector(const std::vector<std::uint8_t>& a_bytes)
		{
			std::vector<std::byte> out(a_bytes.size());
			if (!a_bytes.empty()) {
				std::memcpy(out.data(), a_bytes.data(), a_bytes.size());
			}
			return out;
		}
	}

	bool ResourceExists(std::string_view a_relPath)
	{
		const auto relPath = Util::NormalizeResourcePath(a_relPath);
		if (relPath.empty()) {
			return false;
		}
		ResourceBinaryStream stream{ relPath.c_str() };
		return stream.good();
	}

	namespace GraphManagerClipLoad
	{
		// Unified clip load for the ozz path: a `.af` goes through AFImport (engine-native clip decoded
		// against its species rig); anything else (`.glb`/`.gltf`) through GLTFImport. Both yield
		// an ozz {skeleton, anim} the Graph sampler consumes identically.
		Result Load(const Util::ClipSpec& a_spec, std::string_view a_animId)
		{
			OSF_PROFILE_SCOPE_N("Anim.LoadClip");

			Result out;
			for (std::size_t candidateIndex = 0; candidateIndex < a_spec.candidates.size(); candidateIndex++) {
				const auto& cand = a_spec.candidates[candidateIndex];
				const auto extPath = cand.resource ? std::filesystem::path{ cand.resourcePath } : cand.filePath;
				const bool isAf = Util::ToLower(extPath.extension().string()) == ".af";

				// Pick the rig this .af decodes against from its OWN path. Creature/alien clips live
				// under meshes\actors\<species>\animations and ship their own skeleton.rig (keyed by
				// that rig path so the AFImport cache partitions per species). Human and any loose /
				// unrecognized clip keeps the shipped human rig + its OSF loose-file overrides.
				std::string                               rigKey = "human-skeleton";
				Serialization::AFImport::RigBytesProvider rigProvider = &LoadHumanRigBytes;
				if (isAf) {
					const std::string animPath = cand.resource ? cand.resourcePath : cand.filePath.string();
					const std::string species = Util::SpeciesFromAnimPath(animPath);
					if (!species.empty() && species != "human" && Util::IsKnownSpecies(species)) {
						if (std::string rigRes = Util::RigResourcePathFromAnimPath(animPath); !rigRes.empty()) {
							rigKey = rigRes;
							rigProvider = [rigRes]() { return LoadSpeciesRigBytes(rigRes); };
						}
					}
				}

				if (!cand.resource) {
					if (isAf) {
						auto r = Serialization::AFImport::LoadAnimation(cand.filePath, rigKey, rigProvider);
						if (r.error != Serialization::AFError::kSuccess) {
							out.detail = std::format("af error {}: {}", static_cast<int>(r.error), r.detail);
							out.source = cand.filePath.string();
							return out;
						}
						out.skeleton = std::move(r.skeleton);
						out.anim = std::move(r.anim);
					} else {
						auto r = Serialization::GLTFImport::LoadAnimation(cand.filePath, a_animId);
						if (r.error != Serialization::GLTFError::kSuccess) {
							out.detail = std::format("gltf error {}: {}", static_cast<int>(r.error), r.detail);
							out.source = cand.filePath.string();
							return out;
						}
						out.skeleton = std::move(r.skeleton);
						out.anim = std::move(r.anim);
					}
					out.ok = true;
					out.source = cand.filePath.string();
					if (candidateIndex > 0) {
						WarnImplicitClipFallback(a_spec.display, out.source);
					}
					Serialization::ClipDurations::Record(a_spec.display, a_animId, out.anim->data->duration());
					return out;
				}

				auto bytes = ReadResourceFile(cand.resourcePath);
				if (!bytes) {
					continue;
				}

				if (isAf) {
					auto r = Serialization::AFImport::LoadAnimation(cand.resourcePath, *bytes, rigKey, rigProvider);
					if (r.error != Serialization::AFError::kSuccess) {
						out.detail = std::format("af error {}: {}", static_cast<int>(r.error), r.detail);
						out.source = cand.resourcePath;
						return out;
					}
					out.skeleton = std::move(r.skeleton);
					out.anim = std::move(r.anim);
				} else {
					auto r = Serialization::GLTFImport::LoadAnimation(cand.resourcePath, ToByteVector(*bytes), cand.filePath.parent_path(), a_animId);
					if (r.error != Serialization::GLTFError::kSuccess) {
						out.detail = std::format("gltf error {}: {}", static_cast<int>(r.error), r.detail);
						out.source = cand.resourcePath;
						return out;
					}
					out.skeleton = std::move(r.skeleton);
					out.anim = std::move(r.anim);
				}
				out.ok = true;
				out.source = cand.resourcePath;
				if (candidateIndex > 0) {
					WarnImplicitClipFallback(a_spec.display, out.source);
				}
				Serialization::ClipDurations::Record(a_spec.display, a_animId, out.anim->data->duration());
				return out;
			}

			const auto ext = a_spec.candidates.empty() ? std::string{} :
				Util::ToLower(std::filesystem::path{ a_spec.candidates.front().resourcePath }.extension().string());
			out.source = a_spec.display;
			if (ext == ".af") {
				out.detail = std::format("af error {}: .af missing or unreadable", static_cast<int>(Serialization::AFError::kAfReadFailed));
			} else {
				out.detail = std::format("gltf error {}: file missing, unreadable or gzip decompression failed", static_cast<int>(Serialization::GLTFError::kFileReadFailed));
			}
			return out;
		}
	}
}
