// Starfield `.af` -> ozz-animation import. 
// Decodes the engine-native clip format and the skeleton.rig it maps onto, into an ozz skeleton + runtime animation usable by the Graph sampler
//
// The decode is ported faithfully from CALUMI.Animation (the authoritative reader):
//   - rig layout + bind pose:        CALUMI SFBGS_SkeletonRig.cpp / af_decode.py decode_rig
//   - .af byte layout:               CALUMI SFBGS_Animation.cpp AnimationBlock / af_decode.py decode_af
//   - quaternion/translation dequant:CALUMI SFBGS_AnimationEntries.cpp GetUniversalRotation/Translation
//   - rest-relative -> absolute:     OSF RE docs/af_animation_skeleton_spec.md §8 (local = bind ∘ track)

#include "AFImport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"

namespace OSF::Serialization
{
	namespace
	{
		constexpr double kSqrt1_2 = 0.70710678118654752440;  // 1/sqrt(2)

		// --- byte cursor (buffer-relative alignment, matching CALUMI AlignBufferAndRead) ----------
		struct Cur
		{
			const unsigned char* b = nullptr;
			size_t               size = 0;
			size_t               i = 0;
			bool                 ok = true;

			void align(size_t a)
			{
				if (a > 1 && (i % a) != 0) {
					i += a - (i % a);
				}
			}
			bool ensure(size_t n)
			{
				if (i + n > size) {
					ok = false;
					return false;
				}
				return true;
			}
			// Reads `n` (1 or 2) little-endian bytes into a uint16 after aligning to `a`.
			uint16_t readCounter(size_t n, size_t a)
			{
				align(a);
				if (!ensure(n)) {
					return 0;
				}
				uint16_t v = 0;
				for (size_t k = 0; k < n; k++) {
					v = static_cast<uint16_t>(v | (static_cast<uint16_t>(b[i + k]) << (8 * k)));
				}
				i += n;
				return v;
			}
			template <class T>
			T readPod(size_t a)
			{
				align(a);
				T v{};
				if (!ensure(sizeof(T))) {
					return v;
				}
				std::memcpy(&v, b + i, sizeof(T));
				i += sizeof(T);
				return v;
			}
			// Returns a pointer to `n` bytes (aligned to `a`) and advances; nullptr on overrun.
			const unsigned char* readBytes(size_t n, size_t a)
			{
				align(a);
				if (!ensure(n)) {
					return nullptr;
				}
				const unsigned char* p = b + i;
				i += n;
				return p;
			}
		};

		std::optional<std::vector<unsigned char>> ReadFile(const std::filesystem::path& a_file)
		{
			std::error_code ec;
			const auto sz = std::filesystem::file_size(a_file, ec);
			if (ec) {
				return std::nullopt;
			}
			std::ifstream f(a_file, std::ios::binary);
			if (!f) {
				return std::nullopt;
			}
			std::vector<unsigned char> buf(sz);
			if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) {
				return std::nullopt;
			}
			return buf;
		}

		// --- small quaternion/vector math (ozz convention: Quaternion = x,y,z,w; Float3 = x,y,z) ---
		ozz::math::Quaternion QMul(const ozz::math::Quaternion& a, const ozz::math::Quaternion& b)
		{
			return {
				a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
			};
		}
		ozz::math::Float3 QRotate(const ozz::math::Quaternion& q, const ozz::math::Float3& v)
		{
			// v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
			const float tx = 2.0f * (q.y * v.z - q.z * v.y);
			const float ty = 2.0f * (q.z * v.x - q.x * v.z);
			const float tz = 2.0f * (q.x * v.y - q.y * v.x);
			return {
				v.x + q.w * tx + (q.y * tz - q.z * ty),
				v.y + q.w * ty + (q.z * tx - q.x * tz),
				v.z + q.w * tz + (q.x * ty - q.y * tx)
			};
		}

		// --- rig -----------------------------------------------------------------------------------
		struct RigBone
		{
			std::string         name;
			int32_t             parent = -1;
			ozz::math::Quaternion bindRot = ozz::math::Quaternion::identity();
			ozz::math::Float3     bindTrans = ozz::math::Float3(0.f, 0.f, 0.f);
		};
		struct RigData
		{
			std::vector<RigBone> bones;
			uint16_t             boneCount = 0;
			uint16_t             boneCountAnimated = 0;
			float                lowPrecision = 0.03125f;
			float                highPrecision = 0.00025f;
		};

		// Parse skeleton.rig (80-byte header, 96-byte bone stride). See af_decode.py decode_rig.
		bool ParseRig(const std::vector<unsigned char>& a_buf, RigData& a_out, std::string& a_detail)
		{
			Cur c{ a_buf.data(), a_buf.size(), 0, true };
			c.readPod<int32_t>(1);                       // version
			c.readPod<uint32_t>(1);                      // fileSize
			c.readPod<uint32_t>(1);                      // headerSize
			c.readPod<uint32_t>(1);                      // empty01
			c.readPod<uint32_t>(1);                      // boneMapOffset
			c.readPod<uint32_t>(1);                      // empty02
			c.i += 24;                                    // matchingThree (3x u64)
			a_out.lowPrecision = c.readPod<float>(1);    // @48
			a_out.highPrecision = c.readPod<float>(1);   // @52
			a_out.boneCount = c.readPod<uint16_t>(1);    // @56
			a_out.boneCountAnimated = c.readPod<uint16_t>(1);  // @58
			c.readPod<uint32_t>(1);                      // empty03
			c.i += 16;                                    // end-of-header padding
			if (!c.ok || c.i != 80 || a_out.boneCount == 0 || a_out.boneCount > 4096) {
				a_detail = "rig header invalid";
				return false;
			}

			a_out.bones.resize(a_out.boneCount);
			std::vector<uint64_t> nameOffsets(a_out.boneCount);
			for (uint16_t idx = 0; idx < a_out.boneCount; idx++) {
				const float lrw = c.readPod<float>(1);   // local rotation W
				const float lrx = c.readPod<float>(1);
				const float lry = c.readPod<float>(1);
				const float lrz = c.readPod<float>(1);
				c.i += 16;                               // global rotation (unused)
				const float px = c.readPod<float>(1);
				const float py = c.readPod<float>(1);
				const float pz = c.readPod<float>(1);
				c.readPod<uint32_t>(1);                  // boneType
				nameOffsets[idx] = c.readPod<uint64_t>(1);
				const int32_t parent = c.readPod<int32_t>(1);
				c.i += 4 * 7;                            // twistMqn, twistDrv, pad01, mirror, term05, twistW, pad02
				c.readPod<float>(1);                     // unkScalar
				c.readPod<int32_t>(1);                   // pad03
				auto& bone = a_out.bones[idx];
				bone.parent = parent;
				bone.bindRot = { lrx, lry, lrz, lrw };   // rig stores W,X,Y,Z -> ozz x,y,z,w
				bone.bindTrans = ozz::math::Float3(px, py, pz);
			}
			if (!c.ok) {
				a_detail = "rig truncated reading bones";
				return false;
			}
			// Names: NUL-terminated strings at absolute nameOffset.
			for (uint16_t idx = 0; idx < a_out.boneCount; idx++) {
				const uint64_t off = nameOffsets[idx];
				if (off >= a_buf.size()) {
					a_detail = "rig bone name offset out of range";
					return false;
				}
				size_t end = off;
				while (end < a_buf.size() && a_buf[end] != 0) {
					end++;
				}
				a_out.bones[idx].name.assign(reinterpret_cast<const char*>(a_buf.data() + off), end - off);
			}
			return true;
		}

		std::shared_ptr<Animation::OzzSkeleton> BuildSkeleton(const RigData& a_rig)
		{
			using RawSkeleton = ozz::animation::offline::RawSkeleton;
			RawSkeleton raw;

			// Recursively attach a bone's children (bones whose parent == this index), preserving order.
			auto addJoint = [&](this auto&& self, int32_t a_boneIdx, RawSkeleton::Joint::Children& a_dest) -> void {
				const auto& bone = a_rig.bones[static_cast<size_t>(a_boneIdx)];
				auto& joint = a_dest.emplace_back();
				joint.name = bone.name.c_str();
				joint.transform.translation = bone.bindTrans;
				joint.transform.rotation = bone.bindRot;
				joint.transform.scale = ozz::math::Float3(1.f, 1.f, 1.f);
				for (int32_t child = 0; child < static_cast<int32_t>(a_rig.bones.size()); child++) {
					if (a_rig.bones[static_cast<size_t>(child)].parent == a_boneIdx) {
						self(child, joint.children);
					}
				}
			};

			for (int32_t i = 0; i < static_cast<int32_t>(a_rig.bones.size()); i++) {
				if (a_rig.bones[static_cast<size_t>(i)].parent < 0) {
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

		// --- .af block + dequant -------------------------------------------------------------------
		struct RotPrefix
		{
			int8_t  first = 0, second = 0, third = 0;
			bool    firstFlag = false, secondFlag = false, thirdFlag = false;
			uint8_t count = 0;    // RLE: prefix repeats (count + 1) times ("programming counter")
			uint8_t missing = 3;  // which quaternion component was dropped (0=x 1=y 2=z 3=w)
		};
		struct TransPrefix
		{
			int16_t  x = 0, y = 0, z = 0;
			uint16_t count = 1;   // RLE: prefix repeats `count` times ("classical counter")
		};
		struct AfBlock
		{
			std::vector<uint16_t> rotKeys, transKeys;
			std::vector<std::array<int8_t, 3>> rotEntries, transEntries;
			std::vector<RotPrefix> rotPrefixesFolded;
			std::vector<TransPrefix> transPrefixesFolded;
		};

		int8_t Signed7(unsigned char a_byte)
		{
			const int v = a_byte & 0x7F;
			return static_cast<int8_t>(v >= 64 ? v - 128 : v);
		}

		// Smallest-three quaternion reconstruction (CALUMI GetUniversalRotation). Returns ozz x,y,z,w.
		ozz::math::Quaternion DequantRotation(const RotPrefix& p, const std::array<int8_t, 3>& e)
		{
			const double lowP = kSqrt1_2 / 64.0;
			const double highP = kSqrt1_2 / 16384.0;
			const int s1 = 1 << (3 * (p.firstFlag ? 1 : 0));
			const int s2 = 1 << (3 * (p.secondFlag ? 1 : 0));
			const int s3 = 1 << (3 * (p.thirdFlag ? 1 : 0));
			const double c1 = lowP * p.first + highP * e[0] * s1;
			const double c2 = lowP * p.second + highP * e[1] * s2;
			const double c3 = lowP * p.third + highP * e[2] * s3;
			double rem = 1.0 - c1 * c1 - c2 * c2 - c3 * c3;
			const double c4 = std::sqrt(rem > 0.0 ? rem : 0.0);  // dropped (largest) component, always +
			const float f1 = static_cast<float>(c1), f2 = static_cast<float>(c2), f3 = static_cast<float>(c3), f4 = static_cast<float>(c4);
			switch (p.missing) {
			case 0:  return { f4, f1, f2, f3 };  // x missing
			case 1:  return { f1, f4, f2, f3 };  // y missing
			case 2:  return { f1, f2, f4, f3 };  // z missing
			default: return { f1, f2, f3, f4 };  // w missing
			}
		}

		ozz::math::Float3 DequantTranslation(const TransPrefix& p, const std::array<int8_t, 3>& e, float a_lowPrec, float a_highPrec)
		{
			return ozz::math::Float3(
				p.x * a_lowPrec + e[0] * a_highPrec,
				p.y * a_lowPrec + e[1] * a_highPrec,
				p.z * a_lowPrec + e[2] * a_highPrec);
		}

		// Reads one AnimationBlock (CALUMI SFBGS_Animation.cpp AnimationBlock ctor order).
		bool ReadBlock(Cur& c, size_t a_cSize, size_t a_kSize, AfBlock& a_out)
		{
			const uint16_t rotCount = c.readCounter(a_cSize, a_cSize);
			const uint16_t rotPrefixCount = c.readCounter(a_cSize, a_cSize);
			const uint16_t transCount = c.readCounter(a_cSize, a_cSize);
			const uint16_t transPrefixCount = c.readCounter(a_cSize, a_cSize);
			const uint16_t scalarCount = c.readCounter(a_cSize, a_cSize);
			const uint16_t priorityCount = c.readCounter(a_cSize, a_cSize);

			a_out.rotKeys.resize(rotCount);
			for (auto& k : a_out.rotKeys) {
				k = c.readCounter(a_kSize, a_kSize);
			}
			a_out.transKeys.resize(transCount);
			for (auto& k : a_out.transKeys) {
				k = c.readCounter(a_kSize, a_kSize);
			}
			for (uint16_t i = 0; i < scalarCount; i++) {
				c.readCounter(a_kSize, a_kSize);  // scalar keyframes (unused)
			}
			for (uint16_t i = 0; i < priorityCount; i++) {
				c.readCounter(a_kSize, a_kSize);  // priority keyframes (unused)
			}

			a_out.rotEntries.resize(rotCount);
			for (auto& e : a_out.rotEntries) {
				const unsigned char* p = c.readBytes(3, 1);
				if (p) {
					e = { static_cast<int8_t>(p[0]), static_cast<int8_t>(p[1]), static_cast<int8_t>(p[2]) };
				}
			}
			a_out.rotPrefixesFolded.resize(rotPrefixCount);
			for (auto& pf : a_out.rotPrefixesFolded) {
				const unsigned char* p = c.readBytes(4, 1);
				if (p) {
					pf.first = Signed7(p[0]);
					pf.firstFlag = (p[0] >> 7) & 1;
					pf.second = Signed7(p[1]);
					pf.secondFlag = (p[1] >> 7) & 1;
					pf.third = Signed7(p[2]);
					pf.thirdFlag = (p[2] >> 7) & 1;
					pf.count = p[3] & 0x3F;
					pf.missing = (p[3] >> 6) & 0x3;
				}
			}
			a_out.transEntries.resize(transCount);
			for (auto& e : a_out.transEntries) {
				const unsigned char* p = c.readBytes(3, 1);
				if (p) {
					e = { static_cast<int8_t>(p[0]), static_cast<int8_t>(p[1]), static_cast<int8_t>(p[2]) };
				}
			}
			a_out.transPrefixesFolded.resize(transPrefixCount);
			for (auto& pf : a_out.transPrefixesFolded) {
				const unsigned char* p = c.readBytes(8, 2);
				if (p) {
					std::memcpy(&pf.x, p + 0, 2);
					std::memcpy(&pf.y, p + 2, 2);
					std::memcpy(&pf.z, p + 4, 2);
					std::memcpy(&pf.count, p + 6, 2);
				}
			}
			for (uint16_t i = 0; i < scalarCount; i++) {
				c.readBytes(2, 2);  // scalar entries (int16, unused)
			}
			for (uint16_t i = 0; i < priorityCount; i++) {
				c.readBytes(1, 1);  // priority entries (uint8, unused)
			}
			return c.ok;
		}

		std::vector<RotPrefix> UnfoldRot(const std::vector<RotPrefix>& a_folded)
		{
			std::vector<RotPrefix> out;
			for (const auto& p : a_folded) {
				for (int i = 0; i <= static_cast<int>(p.count); i++) {  // programming counter: count+1 copies
					out.push_back(p);
				}
			}
			return out;
		}
		std::vector<TransPrefix> UnfoldTrans(const std::vector<TransPrefix>& a_folded)
		{
			std::vector<TransPrefix> out;
			for (const auto& p : a_folded) {
				for (uint32_t i = 0; i < p.count; i++) {  // classical counter: count copies
					out.push_back(p);
				}
			}
			return out;
		}

		// --- .af parse (header + preamble skip + atlas walk + blocks) ------------------------------
		struct AfClip
		{
			uint16_t boneCount = 0;
			uint16_t frameCount = 0;
			// blocks[rigBoneIndex] for animated bones; absent (monostate-ish) for skipped bones.
			std::unordered_map<uint16_t, AfBlock> blocks;
		};

		bool ParseAf(const std::vector<unsigned char>& a_buf, AfClip& a_out, std::string& a_detail)
		{
			Cur c{ a_buf.data(), a_buf.size(), 0, true };
			c.i += 8;                          // magic
			c.i += 16;                         // header rotation (W,X,Y,Z) — identity in vanilla
			c.i += 12;                         // header translation
			const unsigned char* flags = c.readBytes(4, 1);
			if (!flags) {
				a_detail = "af header truncated";
				return false;
			}
			const size_t cSize = 1 + ((flags[0] >> 1) & 1);  // keyCounters2Byte
			const size_t kSize = 1 + ((flags[0] >> 2) & 1);  // keyFrameEntries2Byte
			c.readPod<int16_t>(1);             // version
			a_out.boneCount = c.readPod<uint16_t>(1);
			a_out.frameCount = c.readPod<uint16_t>(1);
			const uint16_t atlasN = c.readPod<uint16_t>(1);
			const uint16_t fillCount = c.readPod<uint16_t>(1);
			const uint16_t preambleLen = c.readPod<uint16_t>(1);
			c.i += 12;                         // nZeroFloats
			if (!c.ok || c.i != 64) {
				a_detail = "af header invalid";
				return false;
			}
			if (a_out.boneCount == 0 || atlasN < 2) {
				a_detail = "af has no bones / atlas";
				return false;
			}

			// Preamble: u32 count, then per entry { u16 count; f32 preSet[count*2]; f32 mainSet[frameCount];
			// i16 footer1[count]; i8 footer2[count] }. Parsed only to advance the cursor.
			if (preambleLen > 0) {
				const uint32_t pcount = c.readPod<uint32_t>(4);
				for (uint32_t e = 0; e < pcount && c.ok; e++) {
					const uint16_t pc = c.readPod<uint16_t>(2);
					for (uint32_t k = 0; k < static_cast<uint32_t>(pc) * 2; k++) {
						c.readPod<float>(4);
					}
					for (uint16_t k = 0; k < a_out.frameCount; k++) {
						c.readPod<float>(4);
					}
					for (uint16_t k = 0; k < pc; k++) {
						c.readPod<int16_t>(2);
					}
					for (uint16_t k = 0; k < pc; k++) {
						c.readPod<int8_t>(1);
					}
				}
			}
			// Suffix filler floats, then align to 4 before the atlas.
			for (uint16_t k = 0; k < fillCount; k++) {
				c.readPod<float>(4);
			}
			c.align(4);

			const unsigned char* atlas = c.readBytes(atlasN, 1);
			if (!atlas) {
				a_detail = "af atlas truncated";
				return false;
			}
			uint32_t sumAll = 0;
			for (uint16_t j = 0; j < atlasN; j++) {
				sumAll += atlas[j];
			}
			if (sumAll != a_out.boneCount) {
				a_detail = "af atlas sum != boneCount";
				return false;
			}

			// Walk the RLE atlas: even runs = skipped bones (no block), odd runs = real blocks.
			uint16_t j = 0, k = 0;
			for (uint16_t pos = 0; pos < a_out.boneCount; pos++) {
				while (j < atlasN && k >= atlas[j]) {
					j++;
					k = 0;
				}
				if (j >= atlasN) {
					break;
				}
				if ((j % 2) == 1) {  // odd run -> a real animation block for bone `pos`
					AfBlock blk;
					if (!ReadBlock(c, cSize, kSize, blk)) {
						a_detail = "af block truncated";
						return false;
					}
					a_out.blocks.emplace(pos, std::move(blk));
				}
				k++;
			}
			return c.ok;
		}

		// --- build ozz animation -------------------------------------------------------------------
		std::shared_ptr<Animation::OzzAnimation> BuildAnimation(const AfClip& a_clip, const RigData& a_rig,
			const ozz::animation::Skeleton& a_skeleton)
		{
			// rig bone name -> ozz joint index (SkeletonBuilder reorders joints).
			std::unordered_map<std::string, int> jointByName;
			auto names = a_skeleton.joint_names();
			for (int i = 0; i < a_skeleton.num_joints(); i++) {
				jointByName.emplace(names[i], i);
			}

			const int numJoints = a_skeleton.num_joints();
			auto raw = std::make_unique<ozz::animation::offline::RawAnimation>();
			raw->tracks.resize(numJoints);
			float maxTime = 0.0f;

			for (const auto& [pos, blk] : a_clip.blocks) {
				if (pos >= a_rig.bones.size()) {
					continue;
				}
				const RigBone& bone = a_rig.bones[pos];
				auto it = jointByName.find(bone.name);
				if (it == jointByName.end()) {
					continue;
				}
				auto& track = raw->tracks[it->second];

				// Rotation: dequant rest-relative track, re-base to absolute local (R_abs = R_bind ∘ R_track).
				const auto rotPrefixes = UnfoldRot(blk.rotPrefixesFolded);
				const size_t nRot = std::min({ blk.rotKeys.size(), blk.rotEntries.size(), rotPrefixes.size() });
				for (size_t i = 0; i < nRot; i++) {
					const ozz::math::Quaternion trackRot = DequantRotation(rotPrefixes[i], blk.rotEntries[i]);
					const ozz::math::Quaternion absRot = QMul(bone.bindRot, trackRot);
					const float t = blk.rotKeys[i] / AFImport::kAfFps;
					track.rotations.push_back({ t, absRot });
					maxTime = std::max(maxTime, t);
				}

				// Translation: dequant rest-relative track, re-base (T_abs = T_bind + R_bind · T_track).
				const auto transPrefixes = UnfoldTrans(blk.transPrefixesFolded);
				const size_t nTr = std::min({ blk.transKeys.size(), blk.transEntries.size(), transPrefixes.size() });
				for (size_t i = 0; i < nTr; i++) {
					const ozz::math::Float3 trackTr = DequantTranslation(transPrefixes[i], blk.transEntries[i], a_rig.lowPrecision, a_rig.highPrecision);
					const ozz::math::Float3 absTr = {
						bone.bindTrans.x + QRotate(bone.bindRot, trackTr).x,
						bone.bindTrans.y + QRotate(bone.bindRot, trackTr).y,
						bone.bindTrans.z + QRotate(bone.bindRot, trackTr).z
					};
					const float t = blk.transKeys[i] / AFImport::kAfFps;
					track.translations.push_back({ t, absTr });
					maxTime = std::max(maxTime, t);
				}
			}

			raw->duration = std::max(maxTime, 1.0f / AFImport::kAfFps);

			// Any joint without keyframes (skipped/non-animated bones, twist bones) holds its rest pose.
			for (int i = 0; i < numJoints; i++) {
				auto& track = raw->tracks[i];
				const auto rest = ozz::animation::GetJointLocalRestPose(a_skeleton, i);
				if (track.rotations.empty()) {
					track.rotations.push_back({ 0.0f, rest.rotation });
				}
				if (track.translations.empty()) {
					track.translations.push_back({ 0.0f, rest.translation });
				}
				if (track.scales.empty()) {
					track.scales.push_back({ 0.0f, rest.scale });
				}
			}

			if (!raw->Validate()) {
				return nullptr;
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

		// --- caches (leaked, like GLTFImport: avoids ozz static-destruction order crash) -----------
		std::mutex g_cacheLock;

		struct RigEntry
		{
			std::shared_ptr<RigData>                       rig;
			std::shared_ptr<const Animation::OzzSkeleton>  skeleton;
		};
		auto& RigCache()
		{
			static auto* cache = new std::unordered_map<std::string, RigEntry>();
			return *cache;
		}
		auto& ClipCache()
		{
			static auto* cache = new std::unordered_map<std::string, AFLoadResult>();
			return *cache;
		}

		std::string NormKey(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			auto norm = std::filesystem::weakly_canonical(a_path, ec);
			if (ec) {
				norm = a_path.lexically_normal();
			}
			auto s = norm.string();
			for (auto& ch : s) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return s;
		}
	}

	AFLoadResult AFImport::LoadAnimation(const std::filesystem::path& a_afFile, const std::filesystem::path& a_rigFile)
	{
		const std::string rigKey = NormKey(a_rigFile);
		return LoadAnimation(a_afFile, rigKey, [a_rigFile]() { return ReadFile(a_rigFile); });
	}

	AFLoadResult AFImport::LoadAnimation(const std::filesystem::path& a_afFile, std::string_view a_rigKey,
		const RigBytesProvider& a_rigProvider)
	{
		const std::string rigKey{ a_rigKey };
		const std::string clipKey = NormKey(a_afFile) + '|' + rigKey;

		{
			std::scoped_lock l{ g_cacheLock };
			auto& cc = ClipCache();
			if (auto it = cc.find(clipKey); it != cc.end()) {
				return it->second;
			}
		}

		AFLoadResult result;

		// Rig (cached, shared across clips): parse + ozz skeleton. Fetched via the provider only here.
		std::shared_ptr<RigData> rig;
		std::shared_ptr<const Animation::OzzSkeleton> skeleton;
		{
			std::scoped_lock l{ g_cacheLock };
			if (auto it = RigCache().find(rigKey); it != RigCache().end()) {
				rig = it->second.rig;
				skeleton = it->second.skeleton;
			}
		}
		if (!rig) {
			std::optional<std::vector<unsigned char>> rigBytes;
			if (a_rigProvider) {
				rigBytes = a_rigProvider();
			}
			if (!rigBytes) {
				result.error = AFError::kRigReadFailed;
				result.detail = "skeleton.rig unavailable (no loose file and not found in any BA2)";
				return result;
			}
			auto parsed = std::make_shared<RigData>();
			if (!ParseRig(*rigBytes, *parsed, result.detail)) {
				result.error = AFError::kRigParseFailed;
				return result;
			}
			auto skel = BuildSkeleton(*parsed);
			if (!skel) {
				result.error = AFError::kFailedToBuildSkeleton;
				result.detail = "ozz skeleton build failed from rig (" + std::to_string(parsed->boneCount) + " bones)";
				return result;
			}
			rig = parsed;
			skeleton = skel;
			std::scoped_lock l{ g_cacheLock };
			RigCache().emplace(rigKey, RigEntry{ rig, skeleton });
		}

		// Clip.
		auto afBytes = ReadFile(a_afFile);
		if (!afBytes) {
			result.error = AFError::kAfReadFailed;
			result.detail = ".af missing or unreadable";
			return result;
		}
		AfClip clip;
		if (!ParseAf(*afBytes, clip, result.detail)) {
			result.error = AFError::kAfParseFailed;
			return result;
		}
		auto anim = BuildAnimation(clip, *rig, *skeleton->data);
		if (!anim) {
			result.error = AFError::kFailedToMakeClip;
			result.detail = "ozz animation build failed";
			return result;
		}

		result.skeleton = skeleton;
		result.anim = std::move(anim);
		result.error = AFError::kSuccess;

		std::scoped_lock l{ g_cacheLock };
		ClipCache().emplace(clipKey, result);
		return result;
	}

	void AFImport::ClearCache()
	{
		std::scoped_lock l{ g_cacheLock };
		ClipCache().clear();
		RigCache().clear();
	}
}
