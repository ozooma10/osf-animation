#pragma once

#include <algorithm>
#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace OSF::Util
{
    [[nodiscard]] inline std::uintptr_t GetStarfieldBase() noexcept
    {
        return reinterpret_cast<std::uintptr_t>(::GetModuleHandleA("Starfield.exe"));
    }

    [[nodiscard]] inline std::uintptr_t ToRva(const std::uintptr_t a_address) noexcept
    {
        const auto base = GetStarfieldBase();
        return (base != 0 && a_address >= base) ? (a_address - base) : 0;
    }

    [[nodiscard]] inline bool IsHeapPtr(const std::uintptr_t a_value) noexcept
    {
        return a_value > 0x10000 && a_value < 0x00007fffffffffff && (a_value & 0x3) == 0;
    }

    [[nodiscard]] inline bool IsImagePtr(const std::uintptr_t a_value) noexcept
    {
        return a_value > 0x00007ff000000000 && a_value < 0x00007fffffffffff;
    }

    [[nodiscard]] inline bool IsReadableRange(const std::uintptr_t a_address, const std::size_t a_size)
    {
        if (a_address == 0 || a_size == 0) {
            return false;
        }

        std::uintptr_t cursor = a_address;
        const auto end = a_address + a_size;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
                return false;
            }

            if (mbi.State != MEM_COMMIT) {
                return false;
            }

            const auto protect = mbi.Protect & 0xFF;
            if ((mbi.Protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS) {
                return false;
            }

            const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto regionEnd = regionBase + mbi.RegionSize;
            if (regionEnd <= cursor) {
                return false;
            }

            cursor = (std::min)(end, regionEnd);
        }

        return true;
    }

    [[nodiscard]] inline bool SafeReadQword(const std::uintptr_t a_address, std::uintptr_t& a_value)
    {
        if (!IsReadableRange(a_address, sizeof(std::uintptr_t))) {
            return false;
        }

        __try {
            a_value = *reinterpret_cast<const std::uintptr_t*>(a_address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}
