#pragma once

#include "REX/W32/KERNEL32.h"

#include <cstdint>
#include <cstring>

// Guarded raw-memory reads for the few places OSF Animation must walk live engine
// structures that CommonLibSF doesn't type (the portrait-capture facegen tree). A
// VirtualQuery readability probe per access is slow, but these paths touch only a
// handful of pointers per frame and the alternative — dereferencing a node tree that
// the engine can free the instant a menu closes — is an uncatchable crash.
namespace OSF::Util::Mem
{
	// Base address of Starfield.exe (for RVA math / "is this a code pointer" checks).
	[[nodiscard]] inline std::uintptr_t ImageBase()
	{
		static const std::uintptr_t base = REX::FModule::GetExecutingModule().GetBaseAddress();
		return base;
	}

	// addr - image base, or 0 when addr is below the base (not an image pointer).
	[[nodiscard]] inline std::uintptr_t ToRva(std::uintptr_t a_addr)
	{
		const auto base = ImageBase();
		return a_addr >= base ? a_addr - base : 0;
	}

	// True when [addr, addr+size) is committed and at least readable. Rejects guard
	// pages and PAGE_NOACCESS. Only checks the region containing addr — callers keep
	// their reads inside one struct, so a single query covers each access.
	[[nodiscard]] inline bool IsReadable(std::uintptr_t a_addr, std::size_t a_size)
	{
		if (a_addr == 0 || a_size == 0) {
			return false;
		}
		REX::W32::MEMORY_BASIC_INFORMATION mbi{};
		if (REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_addr), &mbi, sizeof(mbi)) == 0) {
			return false;
		}
		if (mbi.state != REX::W32::MEM_COMMIT) {
			return false;
		}
		constexpr std::uint32_t kReadable = REX::W32::PAGE_READONLY | REX::W32::PAGE_READWRITE |
		                                    REX::W32::PAGE_WRITECOPY | REX::W32::PAGE_EXECUTE_READ |
		                                    REX::W32::PAGE_EXECUTE_READWRITE;
		if ((mbi.protect & kReadable) == 0) {
			return false;
		}
		// The requested span must not run past the end of this committed region.
		const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.baseAddress) + mbi.regionSize;
		return a_addr + a_size <= regionEnd;
	}

	// A pointer into the executable image (a plausible vtable / function pointer).
	[[nodiscard]] inline bool IsImagePtr(std::uintptr_t a_addr)
	{
		return a_addr >= ImageBase() && IsReadable(a_addr, sizeof(void*));
	}

	template <class T>
	[[nodiscard]] inline T Read(std::uintptr_t a_addr, T a_fallback = T{})
	{
		if (!IsReadable(a_addr, sizeof(T))) {
			return a_fallback;
		}
		T v{};
		std::memcpy(&v, reinterpret_cast<const void*>(a_addr), sizeof(T));
		return v;
	}

	[[nodiscard]] inline std::uintptr_t ReadPtr(std::uintptr_t a_addr) { return Read<std::uintptr_t>(a_addr); }
	[[nodiscard]] inline std::uint8_t   ReadU8(std::uintptr_t a_addr) { return Read<std::uint8_t>(a_addr, 0xFF); }
	[[nodiscard]] inline std::uint16_t  ReadU16(std::uintptr_t a_addr) { return Read<std::uint16_t>(a_addr); }
	[[nodiscard]] inline std::uint32_t  ReadU32(std::uintptr_t a_addr) { return Read<std::uint32_t>(a_addr); }

	// RVA of the vtable at *obj (0 when unreadable / not an image pointer).
	[[nodiscard]] inline std::uintptr_t VtableRva(std::uintptr_t a_obj)
	{
		return ToRva(ReadPtr(a_obj));
	}
}
