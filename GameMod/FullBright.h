#pragma once

#include <algorithm>
#include "Nametag.h"
#include "FullBrightProfile.h"

namespace fullbright
{
    struct alignas(8) BridgeData
    {
        volatile LONG enabled = 0;
        LONG profileVersion = 1;
        uintptr_t continuation = 0;
        volatile LONG64 overrides = 0;
    };

    inline unsigned char* bridge = nullptr;
    inline BridgeData* data = nullptr;
    inline nametag::Patch patch{};
    inline volatile LONG enabled = 0;
    inline volatile LONG readiness = 0; // 0 starting, 2 installed, 3 refused/stopping
    inline const char* status = "starting"; // Worker only.
    inline bool stopping = false;
    inline LONG64 finalOverrides = 0;

    inline bool MatchesCode(uintptr_t base, uintptr_t rva, const unsigned char* expected, size_t size)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (rva >= fov::kImageSize || size > fov::kImageSize - rva ||
            VirtualQuery(reinterpret_cast<void*>(base + rva), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ ||
            reinterpret_cast<uintptr_t>(info.AllocationBase) != base ||
            base + rva + size > reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) return false;
        for (size_t offset = 0; offset < size; offset += 64)
            if (!nametag::Matches(base + rva + offset, expected + offset, (std::min)(size - offset, size_t{64})))
                return false;
        return true;
    }

    inline bool ValidateProfile(uintptr_t base, bool supported)
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        return supported && base && nametag::Read(base, &dos, sizeof(dos)) &&
            dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0 && dos.e_lfanew <= 4096 &&
            nametag::Read(base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE &&
            nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt.FileHeader.TimeDateStamp == fov::kImageTimestamp &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt.OptionalHeader.SizeOfImage == fov::kImageSize &&
            MatchesCode(base, kProducerRva, kProducerBytes, sizeof(kProducerBytes)) &&
            MatchesCode(base, kConsumerRva, kConsumerBytes, sizeof(kConsumerBytes));
    }

    inline void BuildBridge(unsigned char* memory, BridgeData* state)
    {
        nametag::Code code(reinterpret_cast<uintptr_t>(memory));
        code.Emit({0x9C, 0x41, 0x53}); // pushfq; push r11
        code.Rip({0x44, 0x8B, 0x1D}, reinterpret_cast<uintptr_t>(&state->enabled));
        code.Emit({0x45, 0x85, 0xDB});
        const size_t disabled = code.Branch(0x84);
        code.Rip({0xF0, 0x48, 0xFF, 0x05}, reinterpret_cast<uintptr_t>(&state->overrides));
        // RSI is the producer's own live 0x44-byte allocation on all paths to
        // this epilogue. Only temporary light texture parameters are written.
        // Native night vision at full strength; omit native darkness dimming.
        // Keep gamma, day cycle, colours and dimension inputs as calculated.
        code.Emit({0xC6, 0x46, 0x24, 0x01});
        code.Emit({0xC7, 0x46, 0x28, 0x00, 0x00, 0x80, 0x3F});
        code.Emit({0xC6, 0x46, 0x2C, 0x00});
        code.Emit({0xC7, 0x46, 0x30, 0x00, 0x00, 0x00, 0x00});
        code.Bind(disabled);
        code.Emit({0x41, 0x5B, 0x9D}); // pop r11; popfq
        code.Emit({0x48, 0x8B, 0x45, 0xC8, 0x0F, 0x28, 0x75, 0xF0}); // displaced epilogue
        code.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->continuation));
        if (code.Size() >= nametag::kDataOffset) throw std::runtime_error("Full Bright bridge size");
        code.Finish(memory);
    }

    inline bool ChangePatch(bool install)
    {
        nametag::FrozenThreads frozen;
        if (!frozen.Freeze() || !frozen.Outside(patch.address, patch.size)) return false;
        if (patch.installed != install && !nametag::WritePatch(patch, install)) return false;
        if (patch.protectionPending)
        {
            DWORD previous = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size, patch.originalProtection, &previous))
                return false;
            patch.protectionPending = false;
        }
        if (patch.cachePending)
        {
            if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), patch.size)) return false;
            patch.cachePending = false;
        }
        return nametag::Matches(patch.address,
            install ? patch.replacement.data() : patch.original.data(), patch.size);
    }

    inline bool Shutdown()
    {
        stopping = true;
        InterlockedExchange(&enabled, 0);
        InterlockedExchange(&readiness, 3);
        if (!bridge) return true;
        InterlockedExchange(&data->enabled, 0);
        if ((patch.installed || patch.protectionPending || patch.cachePending) && !ChangePatch(false))
        { status = "restore-pending"; return false; }
        // No callbacks or return addresses into the DLL. After restoring the
        // entry, exclude threads inside the bridge before releasing its pages.
        nametag::FrozenThreads frozen;
        if (!frozen.Freeze() || !frozen.Outside(reinterpret_cast<uintptr_t>(bridge), nametag::kAllocationSize))
        { status = "bridge-draining"; return false; }
        finalOverrides = InterlockedCompareExchange64(&data->overrides, 0, 0);
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
        data->profileVersion = 1;
        data->continuation = base + kPatchRva + sizeof(kOriginal);
        bool prepared = false;
        try
        {
            BuildBridge(bridge, data);
            DWORD previous = 0;
            prepared = nametag::Jump(patch, base + kPatchRva, kOriginal, sizeof(kOriginal),
                reinterpret_cast<uintptr_t>(bridge)) &&
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

    inline LONG64 OverrideCount()
    {
        return data ? InterlockedCompareExchange64(&data->overrides, 0, 0) : finalOverrides;
    }
}
