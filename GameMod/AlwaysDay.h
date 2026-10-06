#pragma once

#include <algorithm>
#include <cstddef>
#include <new>
#include <tuple>
#include "Nametag.h"
#include "AlwaysDayProfile.h"

namespace alwaysday
{
    // The bridge calls no DLL or game function and holds no world pointer.
    // Only allowlisted renderer callers receive the native noon calculation.
    struct alignas(16) BridgeData
    {
        volatile LONG enabled = 0;
        LONG profileVersion = 3;
        uintptr_t continuation = 0;
        uintptr_t callers[kCallerCount]{};
        volatile LONG64 overrides[kCallerCount]{};
        uintptr_t lightImageCaller = 0;
        uintptr_t starsContinuation = 0;
        uintptr_t cloudContinuation = 0;
        volatile LONG64 starsOverrides = 0;
        volatile LONG64 cloudOverrides = 0;
        uintptr_t reserved = 0; // Explicit padding before the SSE constants.
        alignas(16) uint32_t alphaMask[4]{0, 0, 0, UINT32_MAX};
        alignas(16) uint32_t whiteRgb[4]{0x3F800000, 0x3F800000, 0x3F800000, 0};
    };
    static_assert(offsetof(BridgeData, starsContinuation) == 24 + 16 * kCallerCount);
    static_assert(offsetof(BridgeData, alphaMask) % 16 == 0 && offsetof(BridgeData, whiteRgb) % 16 == 0);
    static_assert(sizeof(BridgeData) <= nametag::kAllocationSize - nametag::kDataOffset);

    inline unsigned char* bridge = nullptr;
    inline BridgeData* data = nullptr;
    inline nametag::Patch patch{};
    inline nametag::Patch starsPatch{}, cloudPatch{};
    inline volatile LONG enabled = 0;
    inline volatile LONG readiness = 0; // 0 starting, 2 installed, 3 refused/stopping
    inline const char* status = "starting"; // Worker only.
    inline bool stopping = false;
    inline std::array<LONG64, kCallerCount> finalOverrides{};
    inline LONG64 finalStarsOverrides = 0, finalCloudOverrides = 0;

    inline bool IsCode(uintptr_t base, uintptr_t rva, size_t size)
    {
        MEMORY_BASIC_INFORMATION info{};
        return rva < fov::kImageSize && size <= fov::kImageSize - rva &&
            VirtualQuery(reinterpret_cast<void*>(base + rva), &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.Protect == PAGE_EXECUTE_READ &&
            reinterpret_cast<uintptr_t>(info.AllocationBase) == base &&
            base + rva + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }

    inline bool Matches(uintptr_t address, const unsigned char* expected, size_t size)
    {
        for (size_t offset = 0; offset < size; offset += 64)
            if (!nametag::Matches(address + offset, expected + offset, (std::min)(size - offset, size_t{64})))
                return false;
        return true;
    }

    inline bool ValidateProfile(uintptr_t base, bool supported)
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!supported || !base || !nametag::Read(base, &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
            !nametag::Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.TimeDateStamp != fov::kImageTimestamp ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage != fov::kImageSize ||
            !IsCode(base, kFunctionRva, sizeof(kFunctionBytes)) ||
            !Matches(base + kFunctionRva, kFunctionBytes, sizeof(kFunctionBytes)) ||
            !IsCode(base, kBrightnessFunctionRva, sizeof(kBrightnessFunctionBytes)) ||
            !Matches(base + kBrightnessFunctionRva, kBrightnessFunctionBytes, sizeof(kBrightnessFunctionBytes)) ||
            !IsCode(base, kLightImageContextRva, sizeof(kLightImageContextBytes)) ||
            !Matches(base + kLightImageContextRva, kLightImageContextBytes, sizeof(kLightImageContextBytes))) return false;
        for (const auto& context : {
            std::make_tuple(kSkyContextRva, kSkyContextBytes, sizeof(kSkyContextBytes)),
            std::make_tuple(kStarCalculationRva, kStarCalculationBytes, sizeof(kStarCalculationBytes)),
            std::make_tuple(kCloudContextRva, kCloudContextBytes, sizeof(kCloudContextBytes)),
            std::make_tuple(kCloudBindingRva, kCloudBindingBytes, sizeof(kCloudBindingBytes))})
            if (!IsCode(base, std::get<0>(context), std::get<2>(context)) ||
                !Matches(base + std::get<0>(context), std::get<1>(context), std::get<2>(context))) return false;
        const unsigned char cloudName[] = "CloudColor";
        if (!Matches(base + 0xEBFFFA7, cloudName, sizeof(cloudName))) return false;
        float cycle = 0, phase = 0;
        if (!nametag::Read(base + 0xE7FEF84, &cycle, sizeof(cycle)) || cycle != 24000.0f ||
            !nametag::Read(base + 0xE7E14EC, &phase, sizeof(phase)) || phase != -0.25f) return false;
        for (const auto& caller : kRenderCallers)
            if (!IsCode(base, caller.contextRva, caller.size) ||
                !Matches(base + caller.contextRva, caller.bytes.data(), caller.size)) return false;
        return true;
    }

    inline void BuildBridge(unsigned char* memory, BridgeData* state)
    {
        nametag::Code code(reinterpret_cast<uintptr_t>(memory));
        // The patched instruction is in a leaf function, before any stack
        // adjustment. Preserve R11 and flags, then read the native return address.
        code.Emit({0x9C, 0x41, 0x53}); // pushfq; push r11
        code.Rip({0x44, 0x8B, 0x1D}, reinterpret_cast<uintptr_t>(&state->enabled));
        code.Emit({0x45, 0x85, 0xDB});
        const size_t disabled = code.Branch(0x84);
        code.Emit({0x4C, 0x8B, 0x5C, 0x24, 0x10}); // r11 = [original rsp]
        std::array<size_t, kCallerCount> accepted{};
        for (size_t i = 0; i < kCallerCount; ++i)
        {
            code.Rip({0x4C, 0x3B, 0x1D}, reinterpret_cast<uintptr_t>(&state->callers[i]));
            const size_t next = code.Branch(0x85);
            size_t otherBrightnessCaller = 0;
            if (kRenderCallers[i].returnRva == kBrightnessReturnRva)
            {
                // Two bridge pushes (0x10), the helper frame and its call
                // put the helper's original return address at rsp + 0x70.
                static_assert(kBrightnessAncestorOffset + 0x10 == 0x70);
                static_assert(kRenderCallers[kCallerCount - 1].returnRva == kBrightnessReturnRva);
                code.Emit({0x4C, 0x8B, 0x5C, 0x24, 0x70});
                code.Rip({0x4C, 0x3B, 0x1D}, reinterpret_cast<uintptr_t>(&state->lightImageCaller));
                otherBrightnessCaller = code.Branch(0x85);
            }
            code.Rip({0xF0, 0x48, 0xFF, 0x05}, reinterpret_cast<uintptr_t>(&state->overrides[i]));
            accepted[i] = code.Jump();
            code.Bind(next);
            if (otherBrightnessCaller) code.Bind(otherBrightnessCaller);
        }
        const size_t otherCaller = code.Jump();
        for (size_t branch : accepted) code.Bind(branch);
        // Native code uses only RAX and XMM2 as inputs after this point.
        // 6000 ticks, partial tick 0 = noon; no level clock is written.
        code.Emit({0xB8, 0x70, 0x17, 0, 0, 0x0F, 0x57, 0xD2});
        code.Bind(disabled);
        code.Bind(otherCaller);
        code.Emit({0x41, 0x5B, 0x9D}); // pop r11; popfq
        code.Emit({0x48, 0x69, 0xC8, 0xF1, 0x19, 0x76, 0x05}); // displaced imul
        code.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->continuation));
        if (code.Size() >= kStarsBridgeOffset) throw std::runtime_error("Always day bridge size");
        code.Finish(memory);

        nametag::Code stars(reinterpret_cast<uintptr_t>(memory + kStarsBridgeOffset));
        stars.Emit({0x9C, 0x41, 0x53});
        stars.Rip({0x44, 0x8B, 0x1D}, reinterpret_cast<uintptr_t>(&state->enabled));
        stars.Emit({0x45, 0x85, 0xDB});
        const size_t nativeStars = stars.Branch(0x84);
        stars.Rip({0xF0, 0x48, 0xFF, 0x05}, reinterpret_cast<uintptr_t>(&state->starsOverrides));
        stars.Emit({0x0F, 0x57, 0xC0}); // star intensity = 0, only for this frame
        stars.Bind(nativeStars);
        stars.Emit({0x41, 0x5B, 0x9D});
        for (unsigned char byte : kStarsOriginal) stars.Emit({byte});
        stars.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->starsContinuation));
        if (stars.Size() >= kCloudBridgeOffset - kStarsBridgeOffset) throw std::runtime_error("Always day stars bridge size");
        stars.Finish(memory + kStarsBridgeOffset);

        nametag::Code clouds(reinterpret_cast<uintptr_t>(memory + kCloudBridgeOffset));
        // Reproduce the native load; alter RGB in a register, never in the
        // source descriptor. Preserve alpha, other registers and flags.
        for (unsigned char byte : kCloudOriginal) clouds.Emit({byte});
        clouds.Emit({0x9C, 0x41, 0x53});
        clouds.Rip({0x44, 0x8B, 0x1D}, reinterpret_cast<uintptr_t>(&state->enabled));
        clouds.Emit({0x45, 0x85, 0xDB});
        const size_t nativeClouds = clouds.Branch(0x84);
        clouds.Rip({0xF0, 0x48, 0xFF, 0x05}, reinterpret_cast<uintptr_t>(&state->cloudOverrides));
        clouds.Rip({0x0F, 0x54, 0x05}, reinterpret_cast<uintptr_t>(state->alphaMask));
        clouds.Rip({0x0F, 0x56, 0x05}, reinterpret_cast<uintptr_t>(state->whiteRgb));
        clouds.Bind(nativeClouds);
        clouds.Emit({0x41, 0x5B, 0x9D});
        clouds.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->cloudContinuation));
        if (clouds.Size() >= nametag::kDataOffset - kCloudBridgeOffset) throw std::runtime_error("Always day clouds bridge size");
        clouds.Finish(memory + kCloudBridgeOffset);
    }

    inline bool ChangePatch(bool install)
    {
        nametag::FrozenThreads frozen;
        if (!frozen.Freeze()) return false;
        const std::array<nametag::Patch*, 3> patches{&patch, &starsPatch, &cloudPatch};
        // Preflight every site before touching any of them. A partial install
        // stays disabled and is restored by Shutdown before bridge release.
        for (const auto* site : patches)
        {
            if (!site->address || !site->size || !frozen.Outside(site->address, site->size) ||
                !nametag::Matches(site->address, site->installed ? site->replacement.data() : site->original.data(), site->size)) return false;
        }
        for (auto* site : patches)
        {
            if (site->installed != install && !nametag::WritePatch(*site, install)) return false;
            if (site->protectionPending)
            {
                DWORD previous = 0;
                if (!VirtualProtect(reinterpret_cast<void*>(site->address), site->size, site->originalProtection, &previous)) return false;
                site->protectionPending = false;
            }
            if (site->cachePending)
            {
                if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site->address), site->size)) return false;
                site->cachePending = false;
            }
            if (!nametag::Matches(site->address, install ? site->replacement.data() : site->original.data(), site->size)) return false;
        }
        return true;
    }

    inline bool Shutdown()
    {
        stopping = true;
        InterlockedExchange(&enabled, 0);
        InterlockedExchange(&readiness, 3);
        if (!bridge) return true;
        InterlockedExchange(&data->enabled, 0);
        const auto pending = [](const nametag::Patch& site) { return site.installed || site.protectionPending || site.cachePending; };
        if ((pending(patch) || pending(starsPatch) || pending(cloudPatch)) && !ChangePatch(false))
        { status = "restore-pending"; return false; }
        // With the entry restored, no new thread can enter the bridge. It has
        // no callback or return address into the DLL; exclude only its own code
        // before releasing it. Native continuations may finish normally.
        nametag::FrozenThreads frozen;
        if (!frozen.Freeze() || !frozen.Outside(reinterpret_cast<uintptr_t>(bridge), nametag::kAllocationSize))
        { status = "bridge-draining"; return false; }
        for (size_t i = 0; i < kCallerCount; ++i)
            finalOverrides[i] = InterlockedCompareExchange64(&data->overrides[i], 0, 0);
        finalStarsOverrides = InterlockedCompareExchange64(&data->starsOverrides, 0, 0);
        finalCloudOverrides = InterlockedCompareExchange64(&data->cloudOverrides, 0, 0);
        if (!VirtualFree(bridge, 0, MEM_RELEASE)) { status = "bridge-release-pending"; return false; }
        bridge = nullptr;
        data = nullptr;
        status = "restored";
        return true;
    }

    inline bool Initialize(uintptr_t base, bool supported)
    {
        stopping = false;
        if (!ValidateProfile(base, supported))
        { status = "profile-refused"; InterlockedExchange(&readiness, 3); return false; }
        bridge = nametag::AllocateNear(base + kPatchRva);
        if (!bridge)
        { status = "allocation-refused"; InterlockedExchange(&readiness, 3); return false; }
        data = reinterpret_cast<BridgeData*>(bridge + nametag::kDataOffset);
        new (data) BridgeData{};
        data->continuation = base + kPatchRva + sizeof(kOriginal);
        data->lightImageCaller = base + kLightImageReturnRva;
        data->starsContinuation = base + kStarsPatchRva + sizeof(kStarsOriginal);
        data->cloudContinuation = base + kCloudPatchRva + sizeof(kCloudOriginal);
        for (size_t i = 0; i < kCallerCount; ++i) data->callers[i] = base + kRenderCallers[i].returnRva;
        bool prepared = false;
        try
        {
            BuildBridge(bridge, data);
            DWORD previous = 0;
            prepared = nametag::Jump(patch, base + kPatchRva, kOriginal, sizeof(kOriginal),
                reinterpret_cast<uintptr_t>(bridge)) &&
                nametag::Jump(starsPatch, base + kStarsPatchRva, kStarsOriginal, sizeof(kStarsOriginal),
                    reinterpret_cast<uintptr_t>(bridge + kStarsBridgeOffset)) &&
                nametag::Jump(cloudPatch, base + kCloudPatchRva, kCloudOriginal, sizeof(kCloudOriginal),
                    reinterpret_cast<uintptr_t>(bridge + kCloudBridgeOffset)) &&
                VirtualProtect(bridge, nametag::kDataOffset, PAGE_EXECUTE_READ, &previous) &&
                FlushInstructionCache(GetCurrentProcess(), bridge, nametag::kDataOffset);
        }
        catch (...) { prepared = false; }
        bool installed = false;
        for (int attempt = 0; prepared && attempt < 3 && !installed; ++attempt)
        {
            installed = ChangePatch(true);
            if (!installed) Sleep(16);
        }
        if (!installed)
        {
            Shutdown();
            status = bridge ? "restore-pending" : "install-refused";
            InterlockedExchange(&readiness, 3);
            return false;
        }
        status = "ready";
        InterlockedExchange(&readiness, 2);
        return true;
    }

    inline bool Toggle()
    {
        if (stopping || !data || InterlockedCompareExchange(&readiness, 0, 0) != 2) return false;
        const LONG next = InterlockedCompareExchange(&enabled, 0, 0) ? 0 : 1;
        InterlockedExchange(&data->enabled, next);
        InterlockedExchange(&enabled, next);
        return true;
    }

    inline LONG64 OverrideCount(size_t index)
    {
        if (index >= kCallerCount) return 0;
        return data ? InterlockedCompareExchange64(&data->overrides[index], 0, 0) : finalOverrides[index];
    }

    inline LONG64 StarsOverrideCount()
    {
        return data ? InterlockedCompareExchange64(&data->starsOverrides, 0, 0) : finalStarsOverrides;
    }

    inline LONG64 CloudOverrideCount()
    {
        return data ? InterlockedCompareExchange64(&data->cloudOverrides, 0, 0) : finalCloudOverrides;
    }
}
