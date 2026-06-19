#pragma once

#include <array>
#include <cstdint>
#include <cstring>

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
}
