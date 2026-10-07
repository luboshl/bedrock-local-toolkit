#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "FovDiscovery.h"

namespace nametag
{
    // This profile was traced in the loaded, pinned PE image, through the
    // native NameTagRenderObject producer and RenderNametagDescription consumer.
    constexpr uintptr_t kOwnGateRva = 0x46B3201;
    constexpr uintptr_t kOwnSkipRva = 0x46B31E0;
    constexpr uintptr_t kDepthGateRva = 0x1D55D5F;
    constexpr uintptr_t kPerspectiveOffset = 0x28;
    constexpr uintptr_t kEnumVtableRva = 0xE778FE0;
    constexpr size_t kAllocationSize = 8192;
    constexpr size_t kDataOffset = 4096;
    constexpr unsigned char kOwnOriginal[] = {0x4C, 0x39, 0xE3, 0x74, 0xDA, 0x48, 0x8B, 0x03};
    // Native BaseActorRenderer::prepareText argument 11: use depth-tested
    // materials for BOTH the text and the background. No actor flag is changed.
    constexpr unsigned char kDepthOriginal[] = {0xC6, 0x44, 0x24, 0x50, 0x00};

    struct alignas(8) BridgeData
    {
        volatile LONG active = 0;
        LONG padding = 0;
        void* volatile ownCallback = nullptr;
        void* volatile depthCallback = nullptr;
        uintptr_t ownContinue = 0;
        uintptr_t ownSkip = 0;
        uintptr_t depthContinue = 0;
    };

    struct Patch
    {
        uintptr_t address = 0;
        size_t size = 0;
        std::array<unsigned char, 8> original{};
        std::array<unsigned char, 8> replacement{};
        bool installed = false;
        DWORD originalProtection = 0;
        bool protectionPending = false;
        bool cachePending = false;
    };

    inline uintptr_t imageBase = 0;
    inline unsigned char* bridge = nullptr;
    inline BridgeData* data = nullptr;
    inline std::array<Patch, 2> patches{};
    inline volatile LONG enabled = 0;
    inline volatile LONG readiness = 0; // 0 starting, 1 waiting, 2 ready, 3 refused
    inline volatile LONG enableByDefaultPending = 0;
    inline volatile LONG64 optionsAddress = 0;
    inline volatile LONG64 ownChecks = 0;
    inline volatile LONG64 shownRear = 0;
    inline volatile LONG64 shownFront = 0;
    inline volatile LONG64 depthSelections = 0;
    inline thread_local uintptr_t frameLocalPlayer = 0;
    inline const char* status = "starting"; // Written/read only by the worker.
    inline bool stopping = false;

    inline bool Read(uintptr_t address, void* output, size_t size)
    {
        SIZE_T count = 0;
        return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
            output, size, &count) && count == size;
    }

    inline bool Matches(uintptr_t address, const unsigned char* expected, size_t size)
    {
        std::array<unsigned char, 64> actual{};
        return size <= actual.size() && Read(address, actual.data(), size) &&
            std::memcmp(actual.data(), expected, size) == 0;
    }

    inline bool IsImageCode(uintptr_t address, size_t size)
    {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.Protect == PAGE_EXECUTE_READ &&
            reinterpret_cast<uintptr_t>(info.AllocationBase) == imageBase &&
            address + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }

    inline bool ValidateProfile(uintptr_t base, bool supported)
    {
        if (!supported || !base) return false;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
            !Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.TimeDateStamp != fov::kImageTimestamp ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage != fov::kImageSize) return false;
        imageBase = base;
        const unsigned char ownContext[] = {0x49, 0x8B, 0x5D, 0x00, 0x4C, 0x39, 0xE3,
            0x74, 0xDA, 0x48, 0x8B, 0x03, 0x48, 0x8B, 0x80, 0xF8, 0, 0, 0, 0x48, 0x89, 0xD9};
        const unsigned char depthContext[] = {0xC6, 0x44, 0x24, 0x60, 0,
            0xC6, 0x44, 0x24, 0x58, 0, 0xC6, 0x44, 0x24, 0x50, 0,
            0xC7, 0x44, 0x24, 0x38, 0, 0, 0x80, 0x3F};
        const unsigned char materialSelector[] = {0x80, 0xBD, 0x60, 0x01, 0, 0, 0,
            0x48, 0x0F, 0x45, 0xC1, 0x49, 0x0F, 0x45, 0xD0, 0x48, 0x01, 0xD8, 0x48, 0x01, 0xDA};
        return IsImageCode(base + kOwnGateRva - 4, sizeof(ownContext)) &&
            IsImageCode(base + kDepthGateRva - 10, sizeof(depthContext)) &&
            IsImageCode(base + 0x1ED337B, sizeof(materialSelector)) &&
            Matches(base + kOwnGateRva - 4, ownContext, sizeof(ownContext)) &&
            Matches(base + kDepthGateRva - 10, depthContext, sizeof(depthContext)) &&
            Matches(base + 0x1ED337B, materialSelector, sizeof(materialSelector));
    }

    inline int Perspective(uintptr_t options)
    {
        uintptr_t vtable = 0, option = 0;
        uintptr_t header[2]{};
        int values[4]{}; // max, min, current, default
        unsigned char key[32]{};
        if (!options || (options & 7) || !Read(options, &vtable, 8) ||
            vtable != imageBase + fov::kOptionsVtableRva ||
            !Read(options + kPerspectiveOffset, &option, 8) || !option || (option & 7) ||
            !Read(option, header, sizeof(header)) || header[0] != imageBase + kEnumVtableRva ||
            !header[1] || (header[1] & 7) ||
            !Read(header[1] + fov::kKeyOffset, key, sizeof(key)) ||
            !Read(option + 0x10, values, sizeof(values))) return -1;
        uintptr_t length = 0, capacity = 0;
        std::memcpy(&length, key + 16, 8);
        std::memcpy(&capacity, key + 24, 8);
        uintptr_t text = 0;
        std::memcpy(&text, key, 8);
        char name[17]{};
        if (length != sizeof(name) - 1 || capacity < length || capacity > 4096 ||
            !Read(text, name, sizeof(name)) || std::memcmp(name, "game_thirdperson", sizeof(name)) != 0 ||
            values[0] != 2 || values[1] != 0 || values[2] < 0 || values[2] > 2 ||
            values[3] < 0 || values[3] > 2) return -1;
        return values[2];
    }

    inline bool __fastcall ShouldInclude(uintptr_t actor, uintptr_t localPlayer)
    {
        // The pointers come from the native actor loop in the current frame.
        frameLocalPlayer = localPlayer;
        if (actor != localPlayer) return true;
        InterlockedIncrement64(&ownChecks);
        if (!actor || !InterlockedCompareExchange(&enabled, 0, 0)) return false;
        const int perspective = Perspective(static_cast<uintptr_t>(InterlockedCompareExchange64(&optionsAddress, 0, 0)));
        if (perspective == 1) InterlockedIncrement64(&shownRear);
        if (perspective == 2) InterlockedIncrement64(&shownFront);
        return perspective == 1 || perspective == 2;
    }

    inline bool __fastcall UseDepth(uintptr_t actor)
    {
        // Keep occlusion even if F7 turns off between inclusion and preparation.
        const bool own = actor && actor == frameLocalPlayer;
        if (own) InterlockedIncrement64(&depthSelections);
        return own;
    }

    // Emit two small, relocatable x64 bridges. Code is RX; callback pointers,
    // counters and destinations are in a separate RW page. No game instruction
    // with RIP-relative data is copied, and no third-party hook engine is used.
    class Code
    {
        uintptr_t address;
        std::vector<unsigned char> bytes;
    public:
        explicit Code(uintptr_t start) : address(start) { bytes.reserve(512); }
        void Emit(std::initializer_list<unsigned char> value) { bytes.insert(bytes.end(), value); }
        void Dword(int32_t value)
        {
            const auto* first = reinterpret_cast<const unsigned char*>(&value);
            bytes.insert(bytes.end(), first, first + 4);
        }
        void Rip(std::initializer_list<unsigned char> op, uintptr_t target)
        {
            Emit(op);
            const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(address + bytes.size() + 4);
            if (delta < INT32_MIN || delta > INT32_MAX) throw std::runtime_error("bridge displacement");
            Dword(static_cast<int32_t>(delta));
        }
        size_t Branch(unsigned char condition)
        {
            Emit({0x0F, condition});
            const size_t position = bytes.size(); Dword(0); return position;
        }
        size_t Jump()
        {
            Emit({0xE9});
            const size_t position = bytes.size(); Dword(0); return position;
        }
        void Bind(size_t position)
        {
            const int32_t delta = static_cast<int32_t>(bytes.size() - position - 4);
            std::memcpy(bytes.data() + position, &delta, 4);
        }
        void Save(bool comparison)
        {
            if (comparison) Emit({0x4C, 0x39, 0xE3}); // cmp rbx,r12: original flags
            Emit({0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
            Emit({0x48, 0x81, 0xEC, 0x80, 0, 0, 0}); // 32-byte shadow space + XMM0..5
            for (unsigned char reg = 0; reg < 6; ++reg)
                Emit({0xF3, 0x0F, 0x7F, static_cast<unsigned char>(0x44 + reg * 8), 0x24,
                    static_cast<unsigned char>(0x20 + reg * 16)});
        }
        void Restore()
        {
            for (unsigned char reg = 0; reg < 6; ++reg)
                Emit({0xF3, 0x0F, 0x6F, static_cast<unsigned char>(0x44 + reg * 8), 0x24,
                    static_cast<unsigned char>(0x20 + reg * 16)});
            Emit({0x48, 0x81, 0xC4, 0x80, 0, 0, 0, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59,
                0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D});
        }
        void Finish(void* output) const { std::memcpy(output, bytes.data(), bytes.size()); }
        size_t Size() const { return bytes.size(); }
    };

    inline void BuildBridges(unsigned char* memory, BridgeData* state)
    {
        const uintptr_t activeAddress = reinterpret_cast<uintptr_t>(&state->active);
        Code own(reinterpret_cast<uintptr_t>(memory));
        own.Save(true);
        own.Rip({0xF0, 0xFF, 0x05}, activeAddress); // count before loading callback
        own.Rip({0x48, 0x8B, 0x05}, reinterpret_cast<uintptr_t>(&state->ownCallback));
        own.Emit({0x48, 0x85, 0xC0});
        const size_t fallback = own.Branch(0x84);
        own.Emit({0x48, 0x89, 0xD9, 0x4C, 0x89, 0xE2, 0xFF, 0xD0, 0x84, 0xC0});
        const size_t skipCallback = own.Branch(0x84);
        const size_t allowedCallback = own.Branch(0x85);
        own.Bind(fallback);
        own.Emit({0x4C, 0x39, 0xE3});
        const size_t skipNative = own.Branch(0x84);
        own.Bind(allowedCallback);
        own.Rip({0xF0, 0xFF, 0x0D}, activeAddress);
        own.Restore();
        own.Emit({0x48, 0x8B, 0x03}); // displaced mov rax,[rbx]
        own.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->ownContinue));
        own.Bind(skipCallback); own.Bind(skipNative);
        own.Rip({0xF0, 0xFF, 0x0D}, activeAddress);
        own.Restore();
        own.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->ownSkip));
        own.Finish(memory);

        Code depth(reinterpret_cast<uintptr_t>(memory + 512));
        depth.Save(false);
        depth.Rip({0xF0, 0xFF, 0x05}, activeAddress);
        depth.Rip({0x48, 0x8B, 0x05}, reinterpret_cast<uintptr_t>(&state->depthCallback));
        depth.Emit({0x48, 0x85, 0xC0});
        const size_t noCallback = depth.Branch(0x84);
        depth.Emit({0x48, 0x8D, 0x4F, 0xF8, 0xFF, 0xD0}); // rdi = Actor + 8
        const size_t haveResult = depth.Jump();
        depth.Bind(noCallback);
        depth.Emit({0x31, 0xC0});
        depth.Bind(haveResult);
        depth.Emit({0x88, 0x84, 0x24, 0x10, 0x01, 0, 0}); // original rsp+50h
        depth.Rip({0xF0, 0xFF, 0x0D}, activeAddress);
        depth.Restore();
        depth.Rip({0xFF, 0x25}, reinterpret_cast<uintptr_t>(&state->depthContinue));
        depth.Finish(memory + 512);
    }

    // All storage and handles are prepared before suspending any game thread.
    // While frozen, use only kernel APIs and fixed buffers (no CRT allocation,
    // logging, loader operations, or calls into game code).
    class FrozenThreads
    {
        struct Entry { DWORD id = 0; HANDLE handle = nullptr; bool suspended = false; };
        std::array<Entry, 2048> threads{};
        size_t count = 0;
    public:
        ~FrozenThreads()
        {
            for (size_t i = 0; i < count; ++i)
                if (threads[i].suspended) ResumeThread(threads[i].handle);
            for (size_t i = 0; i < count; ++i) CloseHandle(threads[i].handle);
        }
        bool Freeze()
        {
            const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return false;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            bool success = Thread32First(snapshot, &entry) != FALSE;
            while (success)
            {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
                {
                    if (count == threads.size()) { success = false; break; }
                    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, entry.th32ThreadID);
                    if (!handle) { success = false; break; }
                    threads[count++] = {entry.th32ThreadID, handle, false};
                }
                if (!Thread32Next(snapshot, &entry))
                { success = GetLastError() == ERROR_NO_MORE_FILES; break; }
            }
            CloseHandle(snapshot);
            if (!success) return false;
            for (size_t i = 0; i < count; ++i)
            {
                if (SuspendThread(threads[i].handle) != static_cast<DWORD>(-1)) threads[i].suspended = true;
                else
                {
                    DWORD exit = STILL_ACTIVE;
                    if (!GetExitCodeThread(threads[i].handle, &exit) || exit == STILL_ACTIVE) return false;
                }
            }
            // Refuse if a thread was created since the first snapshot. All
            // existing threads are stopped, so none can now create another.
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return false;
            success = Thread32First(snapshot, &entry) != FALSE;
            while (success)
            {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
                {
                    bool found = false;
                    for (size_t i = 0; i < count; ++i) if (threads[i].id == entry.th32ThreadID) found = true;
                    if (!found) { success = false; break; }
                }
                if (!Thread32Next(snapshot, &entry))
                { success = GetLastError() == ERROR_NO_MORE_FILES; break; }
            }
            CloseHandle(snapshot);
            return success;
        }
        bool Outside(uintptr_t start, size_t size) const
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (!threads[i].suspended) continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (!GetThreadContext(threads[i].handle, &context)) return false;
                if (context.Rip >= start && context.Rip < start + size) return false;
            }
            return true;
        }
    };

    inline bool WritePatch(Patch& patch, bool install)
    {
        const auto& expected = install ? patch.original : patch.replacement;
        const auto& desired = install ? patch.replacement : patch.original;
        if (!Matches(patch.address, expected.data(), patch.size)) return false;
        DWORD oldProtection = 0;
        void* target = reinterpret_cast<void*>(patch.address);
        if (!VirtualProtect(target, patch.size, PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
        if (!patch.originalProtection) patch.originalProtection = oldProtection;
        patch.protectionPending = true;
        SIZE_T count = 0;
        const bool written = WriteProcessMemory(GetCurrentProcess(), target, desired.data(), patch.size, &count) && count == patch.size;
        if (written) patch.installed = install;
        else
        {
            // Roll back a partial write before allowing any thread to resume.
            const bool rolledBack = WriteProcessMemory(GetCurrentProcess(), target, expected.data(), patch.size, &count) &&
                count == patch.size && Matches(patch.address, expected.data(), patch.size);
            // If rollback fails, conservatively retain the bridge and refuse
            // unload; never treat a partial jump as restored code.
            if (!rolledBack) patch.installed = true;
        }
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), target, patch.size) != FALSE;
        patch.cachePending = !flushed;
        DWORD ignored = 0;
        const bool protectedAgain = VirtualProtect(target, patch.size, patch.originalProtection, &ignored) != FALSE;
        patch.protectionPending = !protectedAgain;
        return written && flushed && protectedAgain && Matches(patch.address, desired.data(), patch.size);
    }

    inline bool Jump(Patch& patch, uintptr_t address, const unsigned char* original, size_t size, uintptr_t destination)
    {
        const int64_t delta = static_cast<int64_t>(destination) - static_cast<int64_t>(address + 5);
        if (delta < INT32_MIN || delta > INT32_MAX || size < 5 || size > 8) return false;
        patch.address = address; patch.size = size;
        std::memcpy(patch.original.data(), original, size);
        patch.replacement.fill(0x90); patch.replacement[0] = 0xE9;
        const int32_t relative = static_cast<int32_t>(delta);
        std::memcpy(patch.replacement.data() + 1, &relative, 4);
        return true;
    }

    inline unsigned char* AllocateNear(uintptr_t target)
    {
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        const uintptr_t granularity = info.dwAllocationGranularity;
        const uintptr_t aligned = target & ~(granularity - 1);
        // Both observed code sites lie within 42 MiB; a nearby allocation can
        // serve both. Fixed RVAs never imply a fixed allocation address.
        for (uintptr_t distance = granularity; distance < 0x40000000; distance += granularity)
        {
            for (uintptr_t candidate : {aligned + distance, aligned > distance ? aligned - distance : 0})
            {
                if (!candidate) continue;
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQuery(reinterpret_cast<void*>(candidate), &region, sizeof(region)) || region.State != MEM_FREE) continue;
                auto* memory = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(candidate), kAllocationSize,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (memory) return memory;
            }
        }
        return nullptr;
    }

    inline bool ChangePatches(bool install)
    {
        FrozenThreads frozen;
        if (!frozen.Freeze()) return false;
        for (const auto& patch : patches)
            if (!frozen.Outside(patch.address, patch.size)) return false;
        // Check all expected bytes before writing either site.
        for (const auto& patch : patches)
            if (patch.installed != install && !Matches(patch.address,
                install ? patch.original.data() : patch.replacement.data(), patch.size)) return false;
        for (auto& patch : patches)
        {
            if (patch.installed != install && !WritePatch(patch, install)) return false;
            if (patch.protectionPending)
            {
                DWORD ignored = 0;
                if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size, patch.originalProtection, &ignored)) return false;
                patch.protectionPending = false;
            }
            if (patch.cachePending)
            {
                if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), patch.size)) return false;
                patch.cachePending = false;
            }
        }
        return true;
    }

    inline bool Shutdown()
    {
        stopping = true;
        InterlockedExchange(&enabled, 0);
        InterlockedExchange(&enableByDefaultPending, 0);
        InterlockedExchange(&readiness, 3);
        InterlockedExchange64(&optionsAddress, 0);
        if (!bridge) return true;
        if ((patches[0].installed || patches[1].installed || patches[0].protectionPending || patches[1].protectionPending ||
            patches[0].cachePending || patches[1].cachePending) && !ChangePatches(false))
        { status = "restore-pending"; return false; }
        InterlockedExchangePointer(&data->ownCallback, nullptr);
        InterlockedExchangePointer(&data->depthCallback, nullptr);
        if (InterlockedCompareExchange(&data->active, 0, 0))
        { status = "callbacks-draining"; return false; }
        // Counters begin before callback loads. After clearing the pointers and
        // draining, no thread can call or return into this DLL. Also exclude
        // threads between a patched jump and the first counter instruction.
        {
            FrozenThreads frozen;
            if (!frozen.Freeze() || !frozen.Outside(reinterpret_cast<uintptr_t>(bridge), kAllocationSize) ||
                InterlockedCompareExchange(&data->active, 0, 0))
            { status = "bridge-draining"; return false; }
            if (!VirtualFree(bridge, 0, MEM_RELEASE)) { status = "bridge-release-pending"; return false; }
            bridge = nullptr; data = nullptr;
        }
        status = "restored";
        return true;
    }

    inline bool Initialize(uintptr_t base, bool supported)
    {
        InterlockedExchange(&enabled, 0);
        InterlockedExchange(&enableByDefaultPending, 0);
        InterlockedExchange64(&optionsAddress, 0);
        stopping = false;
        if (!ValidateProfile(base, supported))
        { status = "profile-refused"; InterlockedExchange(&readiness, 3); return false; }
        bridge = AllocateNear(base + kOwnGateRva);
        if (!bridge) { status = "allocation-refused"; InterlockedExchange(&readiness, 3); return false; }
        data = reinterpret_cast<BridgeData*>(bridge + kDataOffset);
        data->ownCallback = reinterpret_cast<void*>(&ShouldInclude);
        data->depthCallback = reinterpret_cast<void*>(&UseDepth);
        data->ownContinue = base + kOwnGateRva + sizeof(kOwnOriginal);
        data->ownSkip = base + kOwnSkipRva;
        data->depthContinue = base + kDepthGateRva + sizeof(kDepthOriginal);
        try { BuildBridges(bridge, data); }
        catch (...)
        {
            Shutdown(); status = "bridge-build-refused";
            InterlockedExchange(&readiness, 3); return false;
        }
        DWORD previous = 0;
        const bool prepared = Jump(patches[0], base + kOwnGateRva, kOwnOriginal, sizeof(kOwnOriginal), reinterpret_cast<uintptr_t>(bridge)) &&
            Jump(patches[1], base + kDepthGateRva, kDepthOriginal, sizeof(kDepthOriginal), reinterpret_cast<uintptr_t>(bridge + 512)) &&
            VirtualProtect(bridge, kDataOffset, PAGE_EXECUTE_READ, &previous) &&
            FlushInstructionCache(GetCurrentProcess(), bridge, kDataOffset);
        bool installed = false;
        if (prepared)
        {
            for (int attempt = 0; attempt < 3 && !installed; ++attempt)
            {
                installed = ChangePatches(true);
                if (!installed) Sleep(16);
            }
        }
        if (!installed)
        {
            status = "install-refused";
            // A partial installation still owns its bridge until restoration.
            Shutdown();
            status = bridge ? "restore-pending" : "install-refused";
            InterlockedExchange(&readiness, 3);
            return false;
        }
        status = "waiting-options";
        InterlockedExchange(&readiness, 1);
        InterlockedExchange(&enableByDefaultPending, 1);
        return true;
    }

    inline bool EnableByDefault()
    {
        if (stopping || !data || InterlockedCompareExchange(&readiness, 0, 0) != 2) return false;
        InterlockedExchange(&enabled, 1);
        return true;
    }

    inline void BindOptions(uintptr_t options)
    {
        if (!bridge || stopping || !patches[0].installed || !patches[1].installed) return;
        const bool valid = Perspective(options) >= 0;
        InterlockedExchange64(&optionsAddress, valid ? static_cast<LONG64>(options) : 0);
        InterlockedExchange(&readiness, valid ? 2 : 1);
        status = valid ? "ready" : "waiting-options";
        if (valid && InterlockedCompareExchange(&enableByDefaultPending, 0, 0) && EnableByDefault())
            InterlockedExchange(&enableByDefaultPending, 0);
    }

    inline bool Toggle()
    {
        if (stopping || InterlockedCompareExchange(&readiness, 0, 0) != 2) return false;
        InterlockedExchange(&enabled, InterlockedCompareExchange(&enabled, 0, 0) ? 0 : 1);
        return true;
    }
}
