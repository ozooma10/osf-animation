#pragma once

// Minimal RE / REX stand-ins so the engine-independent logic translation units
// (PackRegistry, SceneRegistry, Matchmaker) compile and link in the offline
// `osf-tests` target WITHOUT CommonLibSF or the game.
//
// These are NOT the real engine types — they only model the surface the three
// pure-logic TUs actually touch, with real, controllable behavior so unit tests
// can construct actors/forms and exercise the binding logic. Anything that, in
// the real build, reaches into the running game (the FormID bit-layout via
// TESDataHandler, TESForm::LookupByID) is stubbed to the fail-soft path: the
// data handler reports no plugins, so "Plugin.esm|0xID" form-refs never resolve.
// That is intentional — it lets us regression-test the fail-soft rejection of a
// scene that names a form, which is the behavior we can verify offline. The
// FormID composition itself is RE-sensitive and verified in-game (see docs/RE.md).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// --- REX logging ----------------------------------------------------------------
// The real REX::{INFO,WARN,...} are CTAD struct-templates carrying a source
// location + a consteval std::format_string. We only need the call sites to
// compile and do nothing; a catch-all variadic constructor swallows any args
// (and skips format checking, which the real build still performs).
namespace REX
{
#define OSF_STUB_LOG(NAME)                                  \
	struct NAME                                              \
	{                                                       \
		template <class... A>                               \
		explicit NAME(A&&...) {}                            \
	}

	OSF_STUB_LOG(TRACE);
	OSF_STUB_LOG(DEBUG);
	OSF_STUB_LOG(INFO);
	OSF_STUB_LOG(WARN);
	OSF_STUB_LOG(ERROR);
	OSF_STUB_LOG(CRITICAL);

#undef OSF_STUB_LOG
}

// --- RE engine types ------------------------------------------------------------
namespace RE
{
	using TESFormID = std::uint32_t;

	enum class SEX : std::int32_t
	{
		kNone = -1,
		kMale = 0,
		kFemale = 1
	};

	// An opaque keyword form. Identity (pointer) is all the matcher compares.
	struct BGSKeyword
	{
	};

	// Anything that can carry keywords. Real engine forms multiply-inherit this;
	// here the actorbase (TESNPC) and race derive from it. HasKeyword does an
	// identity scan over the attached keywords — same semantics the matcher relies
	// on (an actor "has" a keyword if its base OR its race carries it).
	struct BGSKeywordForm
	{
		std::vector<BGSKeyword*> keywords;

		bool HasKeyword(BGSKeyword* a_keyword) const
		{
			for (auto* k : keywords) {
				if (k == a_keyword) {
					return true;
				}
			}
			return false;
		}
	};

	struct TESRace : BGSKeywordForm
	{
	};

	struct TESNPC : BGSKeywordForm
	{
		SEX sex = SEX::kNone;

		SEX GetSex() const { return sex; }

		// Mirrors the real TESNPC::HasKeyword(string_view) that hides the inherited
		// BGSKeywordForm::HasKeyword(BGSKeyword*); the matcher qualifies the base
		// call to bypass this, so the body is irrelevant.
		bool HasKeyword(std::string_view) const { return false; }
	};

	struct Actor
	{
		TESNPC* npc = nullptr;
		TESRace* race = nullptr;

		TESNPC* GetNPC() const { return npc; }
	};

	struct TESForm
	{
		// Always "not found" offline: form-refs fail-soft and reject their scene.
		template <class T>
		static T* LookupByID(TESFormID) { return nullptr; }
	};

	struct TESFile
	{
		std::string fileName;
	};

	struct TESDataHandler
	{
		struct CompiledFileCollection
		{
			std::vector<TESFile*> files;
			std::vector<TESFile*> mediumFiles;
			std::vector<TESFile*> smallFiles;
		};

		CompiledFileCollection compiledFileCollection;

		// No data handler offline -> ComposeFormID bails to nullopt (fail-soft).
		static TESDataHandler* GetSingleton() { return nullptr; }
	};

	// PlacementToWorld (Scene.h) returns a braced { x, y, z }.
	struct NiPoint3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};
}
