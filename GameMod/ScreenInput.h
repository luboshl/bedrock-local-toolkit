#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>
#include "FovDiscovery.h"
#include "ScreenInputProfile.h"

namespace screens
{
    inline uintptr_t moduleBase = 0;
    inline bool profileValid = false;
    inline std::atomic<uintptr_t> client{0};

    inline bool Read(uintptr_t address, void* output, size_t size)
    {
        SIZE_T read = 0;
        return address != 0 && size <= (std::numeric_limits<uintptr_t>::max)() - address &&
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                output, size, &read) && read == size;
    }

    template<typename T> bool Read(uintptr_t address, T& output)
    {
        return Read(address, &output, sizeof(output));
    }

    inline bool Type(uintptr_t object, uintptr_t rva)
    {
        uintptr_t vtable = 0;
        return profileValid && Read(object, vtable) && vtable == moduleBase + rva;
    }

    template<size_t N> bool Code(uintptr_t base, uintptr_t rva, const unsigned char (&expected)[N])
    {
        MEMORY_BASIC_INFORMATION memory{};
        std::array<unsigned char, N> actual{};
        return rva < fov::kImageSize && N <= fov::kImageSize - rva &&
            VirtualQuery(reinterpret_cast<const void*>(base + rva), &memory, sizeof(memory)) &&
            memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE && memory.Protect == PAGE_EXECUTE_READ &&
            reinterpret_cast<uintptr_t>(memory.AllocationBase) == base &&
            Read(base + rva, actual) && std::memcmp(actual.data(), expected, N) == 0;
    }

    inline bool Slot(uintptr_t base, uintptr_t table, size_t offset, uintptr_t method)
    {
        MEMORY_BASIC_INFORMATION memory{};
        uintptr_t actual = 0;
        return VirtualQuery(reinterpret_cast<const void*>(base + table), &memory, sizeof(memory)) &&
            memory.Type == MEM_IMAGE && memory.Protect == PAGE_READONLY &&
            reinterpret_cast<uintptr_t>(memory.AllocationBase) == base &&
            Read(base + table + offset, actual) && actual == base + method;
    }

    inline bool Initialize(uintptr_t base, bool supported)
    {
        client.store(0);
        moduleBase = base;
        // The caller verifies the exact package and PE before reaching here.
        profileValid = supported && base &&
            Code(base, kClientStackRva, kClientStackBytes) && Code(base, kGameStackRva, kGameStackBytes) &&
            Code(base, kTopSceneRva, kTopSceneBytes) && Code(base, kSceneNameRva, kSceneNameBytes) &&
            Code(base, kPassthroughRva, kPassthroughBytes) &&
            Code(base, kStackConstructorRva, kStackConstructorBytes) &&
            Code(base, kClientConstructorRva, kClientConstructorBytes) &&
            Code(base, kClientGameRva, kClientGameBytes) &&
            Slot(base, kClientVtableRva, 0x748, kClientStackRva) &&
            Slot(base, kGameVtableRva, 0x3C8, kGameStackRva) &&
            Slot(base, kStackVtableRva, 0x1A8, kTopSceneRva) &&
            Slot(base, kSceneVtableRva, 0x1D0, kSceneNameRva) &&
            Slot(base, kSceneVtableRva, 0x280, kPassthroughRva);
        return profileValid;
    }

    constexpr size_t kMaxScenes = 32;
    struct StackHeader
    {
        uintptr_t vtable, alive, control, begin, end, capacity;
    };
    struct SceneEntry
    {
        uintptr_t owner, scene, control, flags;
    };
    static_assert(sizeof(StackHeader) == 0x30 && sizeof(SceneEntry) == 0x20);

    inline bool StackIdentity(uintptr_t address, StackHeader& header)
    {
        unsigned char alive = 0;
        return Type(address, kStackVtableRva) && Read(address, header) &&
            header.vtable == moduleBase + kStackVtableRva && header.control &&
            Read(header.alive, alive) && alive == 1;
    }

    inline bool Stack(uintptr_t address, StackHeader& header, uint32_t& count)
    {
        return StackIdentity(address, header) &&
            header.begin <= header.end && header.end <= header.capacity &&
            (header.begin & 7) == 0 && (header.end - header.begin) % sizeof(SceneEntry) == 0 &&
            header.capacity - header.begin <= kMaxScenes * sizeof(SceneEntry) &&
            Read(address + 0x88, count) && count <= kMaxScenes &&
            count == (header.end - header.begin) / sizeof(SceneEntry);
    }

    inline bool Client(uintptr_t address, uintptr_t& local, uintptr_t& global)
    {
        uintptr_t game = 0;
        StackHeader header{};
        return Type(address, kClientVtableRva) && Read(address + kClientGameOffset, game) &&
            Type(game, kGameVtableRva) && Read(address + kClientStackOffset, local) &&
            Read(game + kGameStackOffset, global) && StackIdentity(local, header) && StackIdentity(global, header);
    }

    struct Name
    {
        char storage[16];
        size_t size, capacity;
    };
    static_assert(sizeof(Name) == 0x20);
    struct SceneState
    {
        uintptr_t scene = 0, view = 0, tree = 0, root = 0;
        Name name{};
        unsigned char passthrough = 0;
        std::array<char, 64> text{};
    };

    inline bool Scene(uintptr_t address, SceneState& state)
    {
        state = {};
        state.scene = address;
        if (!Type(address, kSceneVtableRva) || !Read(address + kSceneViewOffset, state.view) ||
            !Read(state.view + kViewTreeOffset, state.tree) || !Read(state.tree + kTreeRootOffset, state.root) ||
            !state.view || !state.tree || !state.root ||
            !Read(state.view + kViewPassthroughOffset, state.passthrough) || state.passthrough > 1 ||
            !Read(state.root + kRootNameOffset, state.name) || state.name.size == 0 ||
            state.name.size >= state.text.size() || state.name.capacity < state.name.size) return false;
        if (state.name.capacity < 16)
        {
            if (state.name.capacity != 15 || state.name.size > 15) return false;
            std::memcpy(state.text.data(), state.name.storage, state.name.size + 1);
        }
        else
        {
            uintptr_t data = 0;
            std::memcpy(&data, state.name.storage, sizeof(data));
            if (!Read(data, state.text.data(), state.name.size + 1)) return false;
        }
        return state.text[state.name.size] == 0 &&
            std::memchr(state.text.data(), 0, state.name.size) == nullptr;
    }

    inline bool Gameplay(std::string_view name)
    {
        return name == "hud_screen" || name == "f1_screen" || name == "f3_screen" || name == "zoom_screen";
    }

    // Reproduce the native top-scene walk, with bounded snapshots and a stricter
    // policy: only the known non-interactive debug/toast layers may be skipped.
    // Unknown scene types, pending stack changes and read errors deny shortcuts.
    inline bool StackAllows(uintptr_t address, bool allowEmpty)
    {
        StackHeader before{}, after{};
        uint32_t count = 0, afterCount = 0;
        std::array<SceneEntry, kMaxScenes> entries{}, again{};
        std::array<SceneState, kMaxScenes> states{};
        if (!Stack(address, before, count) ||
            (count && !Read(before.begin, entries.data(), count * sizeof(SceneEntry)))) return false;
        bool allowed = allowEmpty;
        size_t observed = 0;
        for (size_t i = count; i > 0; --i)
        {
            SceneState& state = states[observed++];
            if (!Scene(entries[i - 1].scene, state)) return false;
            const std::string_view name(state.text.data(), state.name.size);
            if (state.passthrough)
            {
                if (name != "debug_screen" && name != "toast_screen") return false;
                continue;
            }
            allowed = Gameplay(name);
            break;
        }
        if (!allowed || !Stack(address, after, afterCount) || count != afterCount ||
            std::memcmp(&before, &after, sizeof(before)) != 0 ||
            (count && (!Read(before.begin, again.data(), count * sizeof(SceneEntry)) ||
                std::memcmp(entries.data(), again.data(), count * sizeof(SceneEntry)) != 0))) return false;
        for (size_t i = 0; i < observed; ++i)
        {
            SceneState current{};
            if (!Scene(states[i].scene, current) || current.view != states[i].view || current.tree != states[i].tree ||
                current.root != states[i].root || current.passthrough != states[i].passthrough ||
                std::memcmp(&current.name, &states[i].name, sizeof(Name)) != 0 || current.text != states[i].text)
                return false;
        }
        return true;
    }

    inline bool AllowsShortcuts()
    {
        const uintptr_t current = client.load();
        uintptr_t local = 0, global = 0, localAgain = 0, globalAgain = 0;
        return Client(current, local, global) && StackAllows(local, false) && StackAllows(global, true) &&
            Client(current, localAgain, globalAgain) && local == localAgain && global == globalAgain;
    }

    struct Discovery
    {
        enum class State { Idle, Running, Verifying, Ready, Refused };
        State state = State::Idle;
        uintptr_t cursor = 0, limit = 0, candidate = 0;
        unsigned int matches = 0;
        ULONGLONG started = 0, finished = 0;
        std::vector<unsigned char> buffer;

        bool Contains(uintptr_t address) const
        {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer.data());
            return address >= begin && address - begin < buffer.size();
        }

        void Refuse()
        {
            client.store(0);
            state = State::Refused;
            finished = GetTickCount64();
        }

        void Advance(uintptr_t otherScratch, size_t otherScratchSize)
        {
            if (!profileValid) return;
            uintptr_t local = 0, global = 0;
            const ULONGLONG now = GetTickCount64();
            if (state == State::Ready)
            {
                if (Client(client.load(), local, global)) return;
                Refuse();
            }
            if (state == State::Refused && now - finished < 2'000) return;
            if (state == State::Idle || state == State::Refused)
            {
                client.store(0);
                SYSTEM_INFO system{};
                GetSystemInfo(&system);
                cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
                limit = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress) + 1;
                candidate = 0;
                matches = 0;
                try { buffer.resize(4 * 1024 * 1024); }
                catch (...) { Refuse(); return; }
                started = now;
                state = State::Running;
            }
            if (state == State::Verifying)
            {
                if (now - finished < 100) return;
                if (!Client(candidate, local, global)) { Refuse(); return; }
                client.store(candidate);
                state = State::Ready;
                return;
            }
            if (now - started > 15'000) { Refuse(); return; }
            size_t budget = 32 * 1024 * 1024;
            unsigned int regions = 1024;
            while (cursor < limit && budget && regions-- && GetTickCount64() - now < 4)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) ||
                    !memory.RegionSize || memory.RegionSize > limit - reinterpret_cast<uintptr_t>(memory.BaseAddress))
                { Refuse(); return; }
                const uintptr_t end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
                if (end <= cursor) { Refuse(); return; }
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE || memory.Protect != PAGE_READWRITE)
                { cursor = end; continue; }
                const size_t bytes = (std::min)(budget, (std::min)(buffer.size(), static_cast<size_t>(end - cursor)));
                if (!Read(cursor, buffer.data(), bytes)) { Refuse(); return; }
                if (!fov::FindOptions(buffer.data(), bytes, cursor, moduleBase + kClientVtableRva,
                    [&](uintptr_t object)
                    {
                        if (Contains(object) || (object >= otherScratch && object - otherScratch < otherScratchSize))
                            return true;
                        if (!Client(object, local, global)) return true;
                        candidate = object;
                        return ++matches <= 1;
                    })) { Refuse(); return; }
                budget -= bytes;
                cursor += bytes;
            }
            if (cursor == limit)
            {
                if (matches != 1) { Refuse(); return; }
                state = State::Verifying;
                finished = GetTickCount64();
            }
        }
    };
}
