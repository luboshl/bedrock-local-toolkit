#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace fov
{
    // Read-only observations of the supported PE image and Options/FloatOption
    // objects. RVAs are relative to Minecraft.Windows.exe, never heap addresses.
    constexpr wchar_t kPackage[] = L"Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe";
    constexpr uint32_t kImageSize = 0x12C01000;
    constexpr uint32_t kImageTimestamp = 0x6AB54E37;
    constexpr uintptr_t kOptionsVtableRva = 0xE7FC490;
    constexpr uintptr_t kValueVtableRva = 0xE778EF0;
    constexpr uintptr_t kFovOptionOffset = 0x188;
    // In the 1.26.5203.0 Options object, Camera sensitivity (mouse) is the
    // FloatOption named ctrl_sensitivity2 / options.sensitivity. Its vtable
    // and strings are matched before reading its live value.
    constexpr uintptr_t kMouseSensitivityVtableRva = 0xE778F20;
    constexpr uintptr_t kMouseSensitivityValueOffset = 0x18;
    constexpr uintptr_t kOptionsSearchBytes = 0x4000;
    constexpr uintptr_t kPatternOffset = 0x10;
    constexpr uintptr_t kOwnerOffset = 0x8;
    constexpr uintptr_t kKeyOffset = 0x188;
    constexpr uintptr_t kDisplayNameOffset = 0x1A8;
    constexpr uintptr_t kCaptionOffset = 0x1D8;
    constexpr float kTolerance = 0.0001f;

    struct Target
    {
        uintptr_t options = 0;
        uintptr_t pattern = 0;
        uintptr_t owner = 0;
    };

    inline bool SameTarget(const Target& left, const Target& right)
    {
        return left.options == right.options && left.pattern == right.pattern && left.owner == right.owner;
    }

    inline bool MayActivate(bool foreground, bool held, bool escape, bool needsRelease,
        bool restorePending, bool stopping, bool ready)
    {
        return foreground && held && !escape && !needsRelease && !restorePending && !stopping && ready;
    }

    inline bool IsPattern(const float* values)
    {
        return std::isfinite(values[0]) && std::isfinite(values[1]) &&
            std::isfinite(values[2]) && std::isfinite(values[3]) && std::isfinite(values[4]) &&
            (std::fabs(values[0] - 30.0f) < kTolerance ||
                std::fabs(values[0] - 10.0f) < kTolerance ||
                std::fabs(values[0] - 1.0f) < kTolerance) &&
            std::fabs(values[1] - 110.0f) < kTolerance &&
            values[2] >= 1.0f && values[2] <= 120.0f &&
            std::fabs(values[3] - 60.0f) < kTolerance &&
            std::fabs(values[4] - 0.001f) < 0.000001f;
    }

    // Match complete 64-bit vtable pointers at 8-byte boundaries, four at a
    // time. Reading snapshots avoids unguarded dereferences of live game heaps.
    template<typename Match>
    bool FindOptions(const unsigned char* data, size_t size, uintptr_t address, uintptr_t vtable, Match match)
    {
        size_t index = static_cast<size_t>((8 - (address & 7)) & 7);
        const __m128i expected = _mm_set1_epi64x(static_cast<long long>(vtable));
        for (; index + 16 <= size; index += 16)
        {
            const __m128i actual = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
            const int equal = _mm_movemask_epi8(_mm_cmpeq_epi32(actual, expected));
            if ((equal & 0xFF) == 0xFF && !match(address + index)) return false;
            if ((equal & 0xFF00) == 0xFF00 && !match(address + index + 8)) return false;
        }
        if (index + 8 <= size)
        {
            uintptr_t value = 0;
            std::memcpy(&value, data + index, sizeof(value));
            if (value == vtable && !match(address + index)) return false;
        }
        return true;
    }
}
