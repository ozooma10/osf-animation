#pragma once

#include "REL/ASM.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

namespace OSF::Util::Hooking
{
	// Verify-before-call gate. Every raw-pointer call into an RE'd engine function in this plugin checks that the first N bytes at the resolved
	// address still match the bytes we disassembled. AddressLib absorbs a pure relocation (function moved); this catches the dangerous case AddressLib
	// can't — the function was recompiled (different prologue/frame/signature) behind a still-valid ID, where calling through our stale signature crashes.

	// Core: compare the prologue at a raw address (e.g. a vtable-resolved pointer). Null address reads as a mismatch.
	template <std::size_t N>
	[[nodiscard]] inline bool PrologueMatches(
		const std::uintptr_t                 a_address,
		const std::array<std::uint8_t, N>&   a_expected)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
		return code && std::memcmp(code, a_expected.data(), N) == 0;
	}

	// Overload for an AddressLib-resolved binding. An unresolved ID (id() == 0,  e.g. a .244-only raw-offset binding on a true .242) reads as a mismatch.
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

	// Feature gate... not sure want to keep
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

	// Installs an entry detour: copies the first N (instruction-boundary-aligned)  bytes of the function into a freshly allocated trampoline gateway, 
	// appends a 14-byte absolute jump back to entry+N, then overwrites the entry with a 5-byte jump to a_thunk. 
	// Returns the gateway (call it to run the original); 0 if the prologue gate failed (caller must not hook). REL::GetTrampoline() must already be allocated (SFSE::AllocTrampoline).
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
