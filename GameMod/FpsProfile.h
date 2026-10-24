#pragma once

#include <array>
#include <cstdint>

#include "Nametag.h"

namespace fpsprofile
{
    constexpr uintptr_t kMinecraftGameUpdateRva = 0x7D31E0;
    constexpr uintptr_t kUpdateCallerRva = 0x7D2F08;
    constexpr size_t kUpdateCallOffset = 3;
    constexpr size_t kUpdateCallLength = 5;

    constexpr std::array<unsigned char, 34> kMinecraftGameUpdateEntry = {
        0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53,
        0x48, 0x81, 0xEC, 0x38, 0x04, 0x00, 0x00, 0x48, 0x8D, 0xAC, 0x24, 0x80,
        0x00, 0x00, 0x00, 0x0F, 0x29, 0xBD, 0xA0, 0x03, 0x00, 0x00
    };

    constexpr std::array<unsigned char, 32> kUpdateCaller = {
        0x48, 0x89, 0xF9, 0xE8, 0xD0, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x8F, 0xD0,
        0x17, 0x00, 0x00, 0xBA, 0x05, 0x00, 0x00, 0x00, 0xE8, 0x4F, 0xA4, 0xFE,
        0xFF, 0x48, 0x8B, 0x9F, 0x60, 0x1E, 0x00, 0x00
    };

    inline bool Matches(uintptr_t moduleBase)
    {
        if (moduleBase == 0 || moduleBase > UINTPTR_MAX - kUpdateCallerRva - kUpdateCaller.size() ||
            moduleBase > UINTPTR_MAX - kMinecraftGameUpdateRva - kMinecraftGameUpdateEntry.size()) return false;
        const uintptr_t caller = moduleBase + kUpdateCallerRva;
        const uintptr_t target = moduleBase + kMinecraftGameUpdateRva;
        if (!nametag::Matches(caller, kUpdateCaller.data(), kUpdateCaller.size()) ||
            !nametag::Matches(target, kMinecraftGameUpdateEntry.data(), kMinecraftGameUpdateEntry.size())) return false;

        int32_t relative = 0;
        if (!nametag::Read(caller + kUpdateCallOffset + 1, &relative, sizeof(relative))) return false;
        const intptr_t destination = static_cast<intptr_t>(caller + kUpdateCallOffset + kUpdateCallLength) + relative;
        return destination >= 0 && static_cast<uintptr_t>(destination) == target;
    }
}
