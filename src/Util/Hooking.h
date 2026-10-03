#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace OSF::Util::Hooking
{
	// Prologue check before calling RE'd engine functions: catches a recompile behind a still-valid
	// AddressLib ID, where a stale signature would crash. Null counts as a mismatch.
	template <std::size_t N>
	[[nodiscard]] inline bool PrologueMatches(
		const std::uintptr_t                 a_address,
		const std::array<std::uint8_t, N>&   a_expected)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
		return code && std::memcmp(code, a_expected.data(), N) == 0;
	}

	// Unresolved ID (id() == 0) counts as a mismatch.
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
