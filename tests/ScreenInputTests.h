#pragma once

namespace
{
    struct ScreenFixture
    {
        std::array<unsigned char, 0x5D8> client{};
        std::array<unsigned char, 0x1D78> game{};
        std::array<unsigned char, 0x90> local{}, global{};
        std::array<screens::SceneEntry, 4> entries{};
        std::array<unsigned char, 0x48> scene{};
        std::array<unsigned char, 0x461> view{};
        std::array<unsigned char, 0x10> tree{};
        std::array<unsigned char, 0x40> root{};
        std::array<char, 64> heapName{};
        unsigned char alive = 1;

        template<typename T, size_t N> static void Store(std::array<unsigned char, N>& data, size_t offset, T value)
        {
            Check(offset + sizeof(value) <= N, "screen fixture bounds");
            std::memcpy(data.data() + offset, &value, sizeof(value));
        }

        void SetName(const char* value, bool heap = false)
        {
            screens::Name name{};
            name.size = std::strlen(value);
            Check(name.size < heapName.size(), "screen name fixture bounds");
            if (heap || name.size > 15)
            {
                std::memcpy(heapName.data(), value, name.size + 1);
                const uintptr_t address = reinterpret_cast<uintptr_t>(heapName.data());
                std::memcpy(name.storage, &address, sizeof(address));
                name.capacity = 63;
            }
            else
            {
                std::memcpy(name.storage, value, name.size + 1);
                name.capacity = 15;
            }
            Store(root, screens::kRootNameOffset, name);
        }

        void SetStack(std::array<unsigned char, 0x90>& stack, bool populated)
        {
            SetStack(stack, entries.data(), populated ? 1u : 0u, entries.size());
        }

        void SetStack(std::array<unsigned char, 0x90>& stack, screens::SceneEntry* data,
            uint32_t count, size_t capacity)
        {
            screens::StackHeader header{};
            header.vtable = screens::moduleBase + screens::kStackVtableRva;
            header.alive = reinterpret_cast<uintptr_t>(&alive);
            header.control = reinterpret_cast<uintptr_t>(&alive); // Never dereferenced by the reader.
            header.begin = reinterpret_cast<uintptr_t>(data);
            header.end = header.begin + count * sizeof(screens::SceneEntry);
            header.capacity = header.begin + capacity * sizeof(screens::SceneEntry);
            Store(stack, 0, header);
            Store(stack, 0x88, count);
        }

        ScreenFixture()
        {
            screens::moduleBase = 0x10000000;
            screens::profileValid = true; // Synthetic objects; Initialize is tested separately.
            Store(client, 0, screens::moduleBase + screens::kClientVtableRva);
            Store(game, 0, screens::moduleBase + screens::kGameVtableRva);
            Store(scene, 0, screens::moduleBase + screens::kSceneVtableRva);
            Store(client, screens::kClientStackOffset, reinterpret_cast<uintptr_t>(local.data()));
            Store(client, screens::kClientGameOffset, reinterpret_cast<uintptr_t>(game.data()));
            Store(game, screens::kGameStackOffset, reinterpret_cast<uintptr_t>(global.data()));
            Store(scene, screens::kSceneViewOffset, reinterpret_cast<uintptr_t>(view.data()));
            Store(view, screens::kViewTreeOffset, reinterpret_cast<uintptr_t>(tree.data()));
            Store(tree, screens::kTreeRootOffset, reinterpret_cast<uintptr_t>(root.data()));
            entries[0].scene = reinterpret_cast<uintptr_t>(scene.data());
            SetStack(local, true);
            SetStack(global, false);
            SetName("hud_screen");
            screens::client.store(reinterpret_cast<uintptr_t>(client.data()));
        }

        ~ScreenFixture()
        {
            alive = 0;
            Store(client, 0, uintptr_t{0});
            screens::client.store(0);
            screens::profileValid = false;
        }
    };

    void TestNativeScreenStacks()
    {
        ScreenFixture fixture, crosshair, debug, toast, menu;
        crosshair.SetName("hud_crosshair_screen");
        debug.SetName("debug_screen");
        toast.SetName("toast_screen");
        const uintptr_t background = screens::moduleBase + screens::kBackgroundVtableRva;
        const uintptr_t opaque = 1; // Below the HUD; never used to authorize gameplay.
        fixture.entries[0].scene = reinterpret_cast<uintptr_t>(&opaque);
        fixture.entries[1].scene = reinterpret_cast<uintptr_t>(crosshair.scene.data());
        fixture.entries[2].scene = reinterpret_cast<uintptr_t>(fixture.scene.data());
        fixture.entries[3].scene = reinterpret_cast<uintptr_t>(menu.scene.data());
        std::array<screens::SceneEntry, 4> shared{};
        shared[0].scene = reinterpret_cast<uintptr_t>(&background);
        shared[1].scene = reinterpret_cast<uintptr_t>(debug.scene.data());
        shared[2].scene = reinterpret_cast<uintptr_t>(toast.scene.data());
        shared[3].scene = reinterpret_cast<uintptr_t>(menu.scene.data());
        fixture.SetStack(fixture.local, fixture.entries.data(), 3, 4);
        fixture.SetStack(fixture.global, shared.data(), 3, 4);
        screens::client.store(reinterpret_cast<uintptr_t>(fixture.client.data()));
        Check(screens::AllowsShortcuts(),
            "live gameplay topology: HUD/crosshair and shared cubemap/debug/toast with zero transition flags");

        g_zoomConfig = {};
        for (const int key : {int{'C'}, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10})
        {
            ObserveShortcut(key, false, screens::AllowsShortcuts());
            ObserveShortcut(key, true, screens::AllowsShortcuts());
            Check(!ShortcutNeedsRelease(key), "all module shortcuts accept a fresh press in the native HUD topology");
        }
        Check(ShouldHideZoomKeyFromGame(screens::AllowsShortcuts()), "native gameplay captures Zoom input");
        for (const char* name : {"chat_screen", "pause_screen", "inventory_screen", "sign_screen",
            "anvil_screen", "unknown_screen"})
        {
            menu.SetName(name);
            fixture.SetStack(fixture.local, fixture.entries.data(), 4, 4);
            const bool allowed = screens::AllowsShortcuts();
            Check(!allowed, "live local menu above HUD denies all shortcuts");
            ObserveShortcut('C', true, allowed);
            Check(ShortcutNeedsRelease('C') && !ShouldHideZoomKeyFromGame(allowed),
                "native chat topology passes its letter and latches the held Zoom shortcut");
            fixture.SetStack(fixture.local, fixture.entries.data(), 3, 4);
            Check(screens::AllowsShortcuts() && ShortcutNeedsRelease('C'),
                "closing native chat requires a release before Zoom can resume");
            ObserveShortcut('C', false, true);
            fixture.SetStack(fixture.global, shared.data(), 4, 4);
            Check(!screens::AllowsShortcuts(), "shared menu above native overlays denies gameplay shortcuts");
            fixture.SetStack(fixture.global, shared.data(), 3, 4);
        }
        for (ScreenFixture* layer : {&fixture, &debug, &toast})
        {
            layer->view[screens::kViewTransitionOffset] = 1;
            Check(!screens::AllowsShortcuts(), "transitioning HUD/debug/toast denies shortcuts");
            layer->view[screens::kViewTransitionOffset] = 0;
        }
        menu.SetName("chat_screen");
        menu.view[screens::kViewTransitionOffset] = 1;
        fixture.SetStack(fixture.local, fixture.entries.data(), 4, 4);
        Check(!screens::AllowsShortcuts(), "closing chat is never skipped to expose the HUD underneath");
        fixture.SetStack(fixture.local, fixture.entries.data(), 3, 4);
        shared[0].scene = reinterpret_cast<uintptr_t>(&opaque);
        Check(!screens::AllowsShortcuts(), "unknown shared bottom scene cannot substitute for the native cubemap");
        shared[0].scene = reinterpret_cast<uintptr_t>(&background);
        shared[1].scene = reinterpret_cast<uintptr_t>(&background);
        Check(!screens::AllowsShortcuts(), "cubemap above the shared bottom is refused");
        shared[1].scene = reinterpret_cast<uintptr_t>(debug.scene.data());
        fixture.entries[2].scene = reinterpret_cast<uintptr_t>(&background);
        Check(!screens::AllowsShortcuts(), "cubemap cannot authorize a player stack");
        fixture.SetStack(fixture.local, false);
        Check(!screens::AllowsShortcuts(), "native shared overlays alone never establish gameplay");
    }

    void TestScreenInput()
    {
        TestNativeScreenStacks();
        Check(!screens::Initialize(0, false) && !screens::AllowsShortcuts(), "unverified screen profile denied");
        // Execute the exact profile byte and vtable checks against synthetic PE
        // memory. MEM_PRIVATE must never substitute for the original image.
        auto* privateCode = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        Check(privateCode != nullptr, "screen profile allocation");
        std::memcpy(privateCode, screens::kTransitionBytes, sizeof(screens::kTransitionBytes));
        DWORD previous = 0;
        Check(VirtualProtect(privateCode, 4096, PAGE_EXECUTE_READ, &previous), "screen profile protection");
        Check(!screens::Code(reinterpret_cast<uintptr_t>(privateCode), 0, screens::kTransitionBytes),
            "matching private executable bytes cannot authorize a profile");
        VirtualFree(privateCode, 0, MEM_RELEASE);

        ScreenFixture fixture;
        for (const char* name : {"hud_screen", "f1_screen", "f3_screen", "zoom_screen"})
        {
            fixture.SetName(name);
            Check(screens::AllowsShortcuts(), "gameplay allowlist accepts exact native names");
            fixture.SetName(name, true);
            Check(screens::AllowsShortcuts(), "heap string gameplay name");
        }
        g_zoomConfig = {};
        for (const char* name : {"chat_screen", "pause_screen", "inventory_screen", "sign_screen",
            "anvil_screen", "start_screen", "hud_screen_extra", "unknown_screen"})
        {
            fixture.SetName(name);
            const bool allowed = screens::AllowsShortcuts();
            Check(!allowed, "text input, menu and unknown screens deny shortcuts");
            RAWINPUT input{};
            input.header.dwType = RIM_TYPEKEYBOARD;
            input.data.keyboard.VKey = 'C';
            input.data.keyboard.MakeCode = 46;
            input.data.keyboard.Message = WM_KEYDOWN;
            RAWINPUT copy = input;
            FilterRawInputZoomKey(input, allowed);
            Check(std::memcmp(&input, &copy, sizeof(input)) == 0, "chat character survives Raw Input unchanged");
            std::array<GameInputKeyState, 2> keys{};
            keys[0].virtualKey = 'C';
            keys[1].virtualKey = 'A';
            const auto originalKeys = keys;
            Check(FilterZoomKey(keys.data(), 2, 2, allowed) == 2 &&
                std::memcmp(keys.data(), originalKeys.data(), sizeof(keys)) == 0,
                "chat character survives both GameInput keyboard paths unchanged");
            Check(!ShouldHideZoomKeyFromGame(allowed), "WinAPI and window-message key filters pass chat input");
            MSG wheel{};
            wheel.message = WM_MOUSEWHEEL;
            wheel.wParam = MAKEWPARAM(0, WHEEL_DELTA);
            Check(!FilterZoomWheelMessage(wheel, allowed) && wheel.message == WM_MOUSEWHEEL,
                "non-gameplay screen retains wheel events");
        }
        fixture.SetName("hud_screen");
        Check(screens::AllowsShortcuts(), "chat close returns to gameplay");
        for (const int key : {int{'C'}, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10})
        {
            ObserveShortcut(key, true, false);
            Check(ShortcutNeedsRelease(key), "shortcut held on a text screen is latched");
            ObserveShortcut(key, true, true);
            Check(ShortcutNeedsRelease(key), "closing chat while held does not activate a shortcut");
            ObserveShortcut(key, false, true);
            Check(!ShortcutNeedsRelease(key), "release clears the latch");
            ObserveShortcut(key, true, true);
            Check(!ShortcutNeedsRelease(key), "fresh gameplay press is allowed");
        }
        RAWINPUT input{};
        input.header.dwType = RIM_TYPEKEYBOARD;
        input.data.keyboard.VKey = 'C';
        FilterRawInputZoomKey(input, screens::AllowsShortcuts());
        Check(input.data.keyboard.VKey == 0 && input.data.keyboard.Flags == RI_KEY_BREAK,
            "gameplay Zoom key remains captured through Raw Input");
        std::array<GameInputKeyState, 2> keys{};
        keys[0].virtualKey = 'C';
        keys[1].virtualKey = 'A';
        Check(FilterZoomKey(keys.data(), 2, 2, screens::AllowsShortcuts()) == 1 && keys[0].virtualKey == 'A',
            "gameplay Zoom key is removed while other GameInput keys survive");
        Check(ShouldHideZoomKeyFromGame(screens::AllowsShortcuts()), "gameplay WinAPI/message key capture");
        ObserveShortcut('C', true, false);
        Check(!ShouldHideZoomKeyFromGame(screens::AllowsShortcuts()), "held chat key is also passed after chat closes");
        ObserveShortcut('C', false, true);

        {
            ScreenFixture menu;
            menu.SetName("pause_screen");
            menu.SetStack(fixture.global, true);
            screens::client.store(reinterpret_cast<uintptr_t>(fixture.client.data()));
            Check(!screens::AllowsShortcuts(), "shared game menu denies shortcuts above a local HUD");
            fixture.SetStack(fixture.global, false);
            menu.SetName("toast_screen");
            menu.view[screens::kViewTransitionOffset] = 0;
            fixture.entries[1].scene = reinterpret_cast<uintptr_t>(menu.scene.data());
            ScreenFixture::Store(fixture.local, 0x20,
                reinterpret_cast<uintptr_t>(fixture.entries.data()) + 2 * sizeof(screens::SceneEntry));
            ScreenFixture::Store(fixture.local, 0x88, uint32_t{2});
            Check(screens::AllowsShortcuts(), "native non-interactive toast above HUD does not block gameplay");
            menu.SetName("chat_screen");
            Check(!screens::AllowsShortcuts(), "unknown layer above HUD still denies shortcuts");
        }
        screens::profileValid = true;
        screens::client.store(reinterpret_cast<uintptr_t>(fixture.client.data()));
        fixture.SetStack(fixture.local, true);
        fixture.SetStack(fixture.global, false);
        fixture.SetName("hud_screen");
        fixture.alive = 0;
        Check(!screens::AllowsShortcuts(), "destroyed stack denies shortcuts");
        fixture.alive = 1;
        ScreenFixture::Store(fixture.local, 0x88, uint32_t{2});
        Check(!screens::AllowsShortcuts(), "inconsistent active count denies shortcuts");
        fixture.SetStack(fixture.local, true);
        ScreenFixture::Store(fixture.scene, 0, uintptr_t{1});
        Check(!screens::AllowsShortcuts(), "unknown scene type denies shortcuts");
        ScreenFixture::Store(fixture.scene, 0, screens::moduleBase + screens::kSceneVtableRva);
        ScreenFixture::Store(fixture.view, screens::kViewTreeOffset, uintptr_t{1});
        Check(!screens::AllowsShortcuts(), "unreadable tree denies shortcuts");
        ScreenFixture::Store(fixture.view, screens::kViewTreeOffset, reinterpret_cast<uintptr_t>(fixture.tree.data()));
        screens::Name bad{};
        bad.capacity = 15;
        bad.size = 64;
        ScreenFixture::Store(fixture.root, screens::kRootNameOffset, bad);
        Check(!screens::AllowsShortcuts(), "malformed name denies shortcuts");
        fixture.SetName("toast_screen");
        fixture.view[screens::kViewTransitionOffset] = 0;
        Check(!screens::AllowsShortcuts(), "non-interactive layer alone cannot establish gameplay");
        Check(screens::StackAllows(reinterpret_cast<uintptr_t>(fixture.local.data()), true),
            "known non-interactive shared layer may be skipped");
        fixture.SetName("chat_screen");
        Check(!screens::StackAllows(reinterpret_cast<uintptr_t>(fixture.local.data()), true),
            "non-gameplay names never authorize shortcuts");
        fixture.SetName("hud_screen");
        fixture.view[screens::kViewTransitionOffset] = 0;
        fixture.SetStack(fixture.local, false);
        Check(!screens::AllowsShortcuts(), "empty player stack denies shortcuts");
        fixture.SetStack(fixture.local, true);
        screens::client.store(0);
        Check(!screens::AllowsShortcuts(), "undiscovered client denies shortcuts");
        screens::client.store(reinterpret_cast<uintptr_t>(fixture.client.data()));
        screens::Discovery discovery;
        discovery.state = screens::Discovery::State::Verifying;
        discovery.candidate = reinterpret_cast<uintptr_t>(fixture.client.data());
        discovery.finished = GetTickCount64() - 100;
        discovery.Advance(0, 0);
        Check(discovery.state == screens::Discovery::State::Ready && screens::AllowsShortcuts(),
            "discovery revalidates the unique candidate before publishing");
        ScreenFixture::Store(fixture.local, 0x88, uint32_t{2});
        discovery.Advance(0, 0);
        Check(discovery.state == screens::Discovery::State::Ready && !screens::AllowsShortcuts(),
            "transient stack mutation blocks input without losing the verified client");
        fixture.SetStack(fixture.local, true);
        Check(screens::AllowsShortcuts(), "stable HUD resumes without another heap scan");
        discovery.state = screens::Discovery::State::Running;
        discovery.started = GetTickCount64() - 15'001;
        discovery.Advance(0, 0);
        Check(discovery.state == screens::Discovery::State::Refused && !screens::AllowsShortcuts(),
            "incomplete discovery cannot publish a client");

        auto complete = [&](screens::Discovery& scan)
        {
            scan.state = screens::Discovery::State::Idle;
            const ULONGLONG deadline = GetTickCount64() + 10'000;
            do
            {
                Check(GetTickCount64() < deadline, "bounded screen discovery completes promptly");
                scan.Advance(reinterpret_cast<uintptr_t>(g_fovScan.buffer.data()), g_fovScan.buffer.size());
                Sleep(1);
            } while (scan.state == screens::Discovery::State::Running ||
                scan.state == screens::Discovery::State::Verifying);
        };
        complete(discovery);
        Check(discovery.state == screens::Discovery::State::Ready &&
            screens::client.load() == reinterpret_cast<uintptr_t>(fixture.client.data()),
            "full scan publishes only one verified client and ignores its scratch snapshot");
        {
            ScreenFixture second;
            complete(discovery);
            Check(discovery.state == screens::Discovery::State::Refused && !screens::AllowsShortcuts(),
                "multiple valid clients refuse shortcuts");
        }
        screens::profileValid = true;
        ScreenFixture::Store(fixture.client, 0, uintptr_t{0});
        complete(discovery);
        Check(discovery.state == screens::Discovery::State::Refused && !screens::AllowsShortcuts(),
            "missing client refuses shortcuts");
        std::puts("Screen input tests passed: live HUD/background/debug/toast topology, native allowlist, player/shared menus and transitions, Raw Input/GameInput passthrough, release latches, malformed state, unique/multiple/missing/timeout discovery.");
    }
}
