#pragma once

#include "REL/ASM.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

namespace OSF::Util::Hooking
{
	// Before we call into a reverse-engineered engine function, check that the first N bytes
	// at the resolved address still match what we disassembled. AddressLib already handles a
	// function simply moving; this catches the dangerous case it can't — the function got
	// recompiled (new prologue/frame/signature) behind a still-valid ID, where calling through
	// our now-stale signature would crash.

	// Compare the prologue at a raw address (e.g. a vtable-resolved pointer). A null address counts as a mismatch.
	template <std::size_t N>
	[[nodiscard]] inline bool PrologueMatches(
		const std::uintptr_t                 a_address,
		const std::array<std::uint8_t, N>&   a_expected)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
		return code && std::memcmp(code, a_expected.data(), N) == 0;
	}

	// Same check for an AddressLib-resolved binding. An unresolved ID (id() == 0, e.g. a
	// .244-only raw-offset binding running on a true .242) counts as a mismatch.
	template <std::size_t N>
	[[nodiscard]] inline bool PrologueMatches(
		const REL::ID                        a_id,
		const std::array<std::uint8_t, N>&   a_expected)
	{
		if (a_id.id() == 0) {
			return false;
		}
		return PrologueMatches(a_id.address(), a_expected);
	}

	// Verify a binding and log whether the feature is available. (Not sure this earns its keep.)
	template <std::size_t N>
	[[nodiscard]] inline bool VerifyFeature(
		const char*                          a_feature,
		const REL::ID                        a_id,
		const std::array<std::uint8_t, N>&   a_expected,
		const char*                          a_detail = "")
	{
		if (!PrologueMatches(a_id, a_expected)) {
			REX::WARN("{} disabled: prologue mismatch at AddressLib ID {} on this runtime {}",
				a_feature, a_id.id(), a_detail);
			return false;
		}
		REX::INFO("{} available: prologue verified at AddressLib ID {}", a_feature, a_id.id());
		return true;
	}

	template <std::size_t N>
	[[nodiscard]] inline bool VerifyExpectedBytes(
		const char*                          a_label,
		const std::uintptr_t                 a_address,
		const std::array<std::uint8_t, N>&   a_expected)
	{
		const auto* actual = reinterpret_cast<const std::uint8_t*>(a_address);
		if (a_address && std::equal(a_expected.begin(), a_expected.end(), actual)) {
			return true;
		}

		std::string actualBytes;
		if (a_address) {
			for (std::size_t i = 0; i < N; ++i) {
				if (i != 0) {
					actualBytes += ' ';
				}
				actualBytes += std::format("{:02X}", actual[i]);
			}
		} else {
			actualBytes = "<null address>";
		}
		REX::WARN("{} bytes drifted at 0x{:X}: {}", a_label, a_address, actualBytes);
		return false;
	}

	// Installs an entry detour: copy the function's first N bytes (aligned to an instruction
	// boundary) into a freshly allocated trampoline gateway, append a 14-byte absolute jump back
	// to entry+N, then overwrite the entry with a 5-byte jump to a_thunk. Returns the gateway —
	// call it to run the original — or 0 if the prologue check failed (in which case don't hook).
	// The trampoline must already be allocated via SFSE::AllocTrampoline.
	template <std::size_t N, class T>
	[[nodiscard]] inline std::uintptr_t InstallEntryHookWithGateway(
		const REL::Offset                    a_offset,
		const char*                          a_label,
		const std::array<std::uint8_t, N>&   a_expectedBytes,
		T                                    a_thunk)
	{
		REL::Relocation<std::uintptr_t> relocation{ a_offset };
		const auto                      entryAddress = relocation.address();
		if (!VerifyExpectedBytes(a_label, entryAddress, a_expectedBytes)) {
			return 0;
		}

		auto& trampoline = REL::GetTrampoline();
		auto* gateway = static_cast<std::byte*>(trampoline.allocate(N + sizeof(REL::ASM::JMP14)));
		std::memcpy(gateway, reinterpret_cast<const void*>(entryAddress), N);

		const REL::ASM::JMP14 jumpBack{ entryAddress + N };
		std::memcpy(gateway + N, &jumpBack, sizeof(jumpBack));

		relocation.write_jmp<5>(a_thunk);
		return reinterpret_cast<std::uintptr_t>(gateway);
	}
}
