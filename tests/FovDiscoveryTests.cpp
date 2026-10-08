// Executes the real discovery/verification/restore code in this test process.
// No Minecraft process is opened and StartGameMod is never called.
#include "../GameMod/GameMod.cpp"
#include <array>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct TemporaryToolkitConfig
    {
        wchar_t path[MAX_PATH]{};

        TemporaryToolkitConfig()
        {
            wchar_t directory[MAX_PATH]{};
            const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(directory)), directory);
            Check(length != 0 && length < std::size(directory) &&
                GetTempFileNameW(directory, L"blt", 0, path) != 0, "temporary config path");
        }

        ~TemporaryToolkitConfig()
        {
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            DeleteFileW(path);
        }

        void Set(const wchar_t* section, const wchar_t* key, const wchar_t* value)
        {
            Check(WritePrivateProfileStringW(section, key, value, path) != FALSE, "write temporary config");
        }

        std::wstring Path() const { return path; }
    };

    void TestToolkitConfig()
    {
        TemporaryToolkitConfig config;
        Check(LoadZoomConfigFromPath(config.Path()), "load defaults without a config");
        Check(g_zoomConfig.firstPersonFov == 15.0f && g_zoomConfig.thirdPersonFov == 28.0f,
            "separate default FOVs for each perspective");
        config.Set(L"Zoom", L"Fov", L"33.5");
        config.Set(L"Zoom", L"TransitionDurationMs", L"275");
        config.Set(L"Zoom", L"MouseSensitivity", L"27.5");
        config.Set(L"Shortcuts", L"Zoom", L"Z");
        config.Set(L"Shortcuts", L"Nametag", L"F1");
        config.Set(L"Shortcuts", L"AlwaysDay", L"F2");
        config.Set(L"Shortcuts", L"FullBright", L"F3");
        config.Set(L"Shortcuts", L"StatusIndicator", L"F4");
        config.Set(L"Shortcuts", L"SafeDetach", L"F5");
        Check(LoadZoomConfigFromPath(config.Path()), "load [Zoom] and [Shortcuts]");
        Check(g_zoomConfig.firstPersonFov == 33.5f && g_zoomConfig.thirdPersonFov == 33.5f &&
            g_zoomConfig.transitionDurationMs == 275 &&
            g_zoomConfig.mouseSensitivity == 27.5f, "Zoom settings parsed");
        config.Set(L"Zoom", L"FirstPersonFov", L"15.5");
        Check(LoadZoomConfigFromPath(config.Path()) &&
            g_zoomConfig.firstPersonFov == 15.5f && g_zoomConfig.thirdPersonFov == 33.5f,
            "first-person FOV overrides only the legacy first-person fallback");
        config.Set(L"Zoom", L"ThirdPersonFov", L"28.5");
        Check(LoadZoomConfigFromPath(config.Path()) &&
            g_zoomConfig.firstPersonFov == 15.5f && g_zoomConfig.thirdPersonFov == 28.5f,
            "per-perspective FOV settings override the legacy fallback");

        float target = 0.0f;
        Check(ZoomFovForPerspective(g_zoomConfig, 0, target) && target == 15.5f,
            "first-person selects its configured FOV");
        Check(ZoomFovForPerspective(g_zoomConfig, 1, target) && target == 28.5f &&
            ZoomFovForPerspective(g_zoomConfig, 2, target) && target == 28.5f,
            "both third-person views select their configured FOV");
        Check(!ZoomFovForPerspective(g_zoomConfig, -1, target),
            "unknown camera perspective is refused");
        Check(g_zoomConfig.zoomKey == 'Z' && g_zoomConfig.nametagKey == VK_F1 &&
            g_zoomConfig.alwaysDayKey == VK_F2 && g_zoomConfig.fullBrightKey == VK_F3 &&
            g_zoomConfig.indicatorKey == VK_F4 && g_zoomConfig.exitKey == VK_F5,
            "all shortcut keys parsed");

        config.Set(L"Shortcuts", L"StatusIndicator", L"NotAKey");
        Check(!LoadZoomConfigFromPath(config.Path()), "unsupported shortcut rejected");
        config.Set(L"Shortcuts", L"StatusIndicator", L"F4");
        config.Set(L"Shortcuts", L"SafeDetach", L"F4");
        Check(!LoadZoomConfigFromPath(config.Path()), "duplicate shortcuts rejected");
    }

    void TestZoomTransitionInterpolation()
    {
        Check(InterpolateZoomFov(70.0f, 10.0f, 0, 50) == 70.0f,
            "zoom transition begins at its starting FOV");
        Check(std::fabs(InterpolateZoomFov(70.0f, 10.0f, 10, 40) - 60.625f) < 0.001f,
            "zoom transition eases in");
        Check(std::fabs(InterpolateZoomFov(70.0f, 10.0f, 20, 40) - 40.0f) < 0.001f,
            "zoom transition reaches the midpoint");
        Check(std::fabs(InterpolateZoomFov(70.0f, 10.0f, 30, 40) - 19.375f) < 0.001f,
            "zoom transition eases out");
        Check(InterpolateZoomFov(70.0f, 10.0f, 50, 50) == 10.0f &&
            InterpolateZoomFov(70.0f, 10.0f, 10'000, 10'000) == 10.0f,
            "zoom transition reaches its target for different durations");
        Check(InterpolateZoomFov(70.0f, 10.0f, 2'500, 10'000) ==
            InterpolateZoomFov(70.0f, 10.0f, 10, 40),
            "zoom easing is independent of the configured duration");
    }

    void TestOverlayVisibilityPositionFlags()
    {
        Check((OverlayPositionFlags(false) & SWP_SHOWWINDOW) == 0,
            "positioning a hidden overlay does not show it");
        Check((OverlayPositionFlags(true) & SWP_SHOWWINDOW) != 0,
            "positioning a visible overlay keeps it shown");
    }

    void TestDeferredZoomDiagnostics()
    {
        TemporaryToolkitConfig log;
        const std::wstring originalPath = g_statusLogPath;
        g_statusLogPath = log.Path();
        g_zoomLogEventCount = 0;
        LogZoomStatus("deferred-test");
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        Check(GetFileAttributesExW(log.path, GetFileExInfoStandard, &attributes) &&
            attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow == 0,
            "input diagnostic does not open or write a file");
        AcquireSRWLockExclusive(&g_zoomLogLock);
        LogZoomStatus("contended-test");
        ReleaseSRWLockExclusive(&g_zoomLogLock);
        Check(g_zoomLogEventCount == 1, "contended diagnostics return without blocking input");
        for (int i = 0; i < 100; ++i) LogZoomStatus("overflow-test");
        Check(g_zoomLogEventCount == g_zoomLogEvents.size(), "diagnostic backlog is bounded");
        FlushZoomStatusLogs();
        Check(g_zoomLogEventCount == 0 &&
            GetFileAttributesExW(log.path, GetFileExInfoStandard, &attributes) && attributes.nFileSizeLow > 0,
            "worker drains queued diagnostics to disk");
        g_statusLogPath = originalPath;
    }

    struct MessageThread
    {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = nullptr;
        HWND window = nullptr;
        DWORD id = 0;

        static DWORD WINAPI Run(void* context)
        {
            auto& self = *static_cast<MessageThread*>(context);
            self.window = CreateWindowExW(0, L"STATIC", L"Input hook test", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            SetEvent(self.ready);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
                DispatchMessageW(&message);
            if (self.window != nullptr) DestroyWindow(self.window);
            return 0;
        }

        MessageThread()
        {
            Check(ready != nullptr, "message thread event");
            thread = CreateThread(nullptr, 0, Run, this, 0, &id);
            Check(thread != nullptr && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && window != nullptr,
                "synthetic window and message thread start");
        }

        ~MessageThread()
        {
            PostThreadMessageW(id, WM_QUIT, 0, 0);
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
            CloseHandle(ready);
        }
    };

    void TestMessageHookOwnership()
    {
        MessageThread first, replacement;
        Check(InstallGameInputMessageHooks(first.window) == 1 && g_gameInputHooks.size() == 1 &&
            g_gameInputHooks.front().threadId == first.id,
            "message filtering hooks only the owning game window thread");
        const HHOOK hook = g_gameInputHooks.front().handle;
        Check(InstallGameInputMessageHooks(first.window) == 0 && g_gameInputHooks.front().handle == hook,
            "repeated maintenance keeps the existing hook");
        Check(InstallGameInputMessageHooks(nullptr) == 0 && g_gameInputHooks.front().handle == hook,
            "missing window does not install hooks on unrelated threads");
        Check(InstallGameInputMessageHooks(replacement.window) == 1 && g_gameInputHooks.size() == 1 &&
            g_gameInputHooks.front().threadId == replacement.id,
            "window owner replacement retires the old hook");
        UnhookWindowsHookEx(g_gameInputHooks.front().handle);
        g_gameInputHooks.clear();
    }

    void TestWorkerWaitMessages()
    {
        // A queued message must interrupt both normal and fallback waits;
        // otherwise the low-level keyboard hook waits for the timer.
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        Check(timer != nullptr, "high-resolution worker timer");
        for (HANDLE waitTimer : {timer, static_cast<HANDLE>(nullptr)})
        {
            Check(PostThreadMessageW(GetCurrentThreadId(), WM_APP, 0, 0) != FALSE, "queue worker message");
            const double started = ZoomTimeMs();
            WaitForWorkerInput(waitTimer, 2000);
            Check(ZoomTimeMs() - started < 1000.0 &&
                PeekMessageW(&message, nullptr, WM_APP, WM_APP, PM_REMOVE),
                "worker wait wakes for input before its deadline");
        }
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }

    struct Fixture
    {
        unsigned char* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        fov::Target target;

        void Pointer(size_t offset, uintptr_t value) { std::memcpy(memory + offset, &value, 8); }
        void String(size_t offset, const char* value)
        {
            const size_t length = std::strlen(value);
            if (length < 16)
            {
                std::memcpy(memory + offset, value, length + 1);
                Pointer(offset + 24, 15);
            }
            else
            {
                std::memcpy(memory + 1152, value, length + 1);
                Pointer(offset, reinterpret_cast<uintptr_t>(memory + 1152));
                Pointer(offset + 24, 31);
            }
            Pointer(offset + 16, length);
        }

        Fixture()
        {
            Check(memory != nullptr, "fixture allocation");
            const uintptr_t start = reinterpret_cast<uintptr_t>(memory);
            target = {start, start + 1040, start + 512};
            Pointer(0, g_gameModuleBase + fov::kOptionsVtableRva);
            Pointer(fov::kFovOptionOffset, start + 1024);
            Pointer(1024, g_gameModuleBase + fov::kValueVtableRva);
            Pointer(1032, target.owner);
            String(512 + fov::kKeyOffset, "gfx_field_of_view");
            String(512 + fov::kDisplayNameOffset, "fieldOfView");
            String(512 + fov::kCaptionOffset, "options.fov");
            const float values[] = {30, 110, 70, 60, .001f};
            std::memcpy(memory + 1040, values, sizeof(values));
        }
        ~Fixture() { VirtualFree(memory, 0, MEM_RELEASE); }
        Fixture(const Fixture&) = delete;
        Fixture& operator=(const Fixture&) = delete;
        float Value() const { float value; std::memcpy(&value, memory + 1048, 4); return value; }
        void Value(float value) { std::memcpy(memory + 1048, &value, 4); }
    };

    void TestZoomWheelTransitionRetargeting()
    {
        Fixture fixture;
        const ULONGLONG originalDuration = g_zoomConfig.transitionDurationMs;
        fixture.Value(50.0f);
        g_zoom = {};
        g_zoom.target = fixture.target;
        g_zoom.originalValue = 70.0f;
        g_zoom.active = true;
        g_zoom.originalMinimumValue = kZoomMinimumFov;
        g_zoom.currentZoomValue = 45.0f;
        g_zoom.displayedValue = 50.0f;
        g_zoom.transitionStartValue = 70.0f;
        g_zoom.transitionTargetValue = 45.0f;
        g_zoom.transitionStartedAt = ZoomTimeMs() - 25;
        g_zoom.transitionActive = true;
        g_zoomConfig.transitionDurationMs = 100;
        g_zoomRestorePending = false;

        Check(ScheduleZoomWheelTransition(g_zoom, 1, ZoomTimeMs()),
            "wheel step schedules a new transition");
        Check(g_zoom.displayedValue == 50.0f && fixture.Value() == 50.0f,
            "wheel step leaves the displayed FOV unchanged immediately");
        Check(g_zoom.transitionTargetValue == 40.0f && g_zoom.transitionActive &&
            g_zoom.transitionStartValue == 50.0f,
            "wheel step retargets the active transition from the displayed FOV");

        g_zoom.transitionStartedAt = ZoomTimeMs() - 50;
        AdvanceZoomTransition();
        Check(fixture.Value() > 40.0f && fixture.Value() < 50.0f,
            "retargeted transition advances toward its new target");

        g_zoom.transitionStartedAt = ZoomTimeMs() - g_zoomConfig.transitionDurationMs;
        AdvanceZoomTransition();
        Check(fixture.Value() == 40.0f && g_zoom.currentZoomValue == 40.0f &&
            !g_zoom.transitionActive,
            "retargeted transition reaches and records its new target");
        g_zoom = {};
        g_zoomConfig.transitionDurationMs = originalDuration;
        g_zoomRestorePending = false;
    }

    void TestZoomTransitionTiming()
    {
        Fixture fixture;
        const ULONGLONG originalDuration = g_zoomConfig.transitionDurationMs;
        g_zoomConfig.transitionDurationMs = 180;
        g_zoom = {};
        g_zoom.target = fixture.target;
        g_zoom.active = true;
        g_zoom.displayedValue = 70.0f;
        g_zoom.transitionStartValue = 70.0f;
        g_zoom.transitionTargetValue = 15.0f;
        g_zoom.transitionStartedAt = 1000.0;
        g_zoom.transitionActive = true;
        g_zoomRestorePending = false;
        AdvanceZoomTransition(1000.5);
        const float first = fixture.Value();
        AdvanceZoomTransition(1001.0);
        Check(first < 70.0f && fixture.Value() < first,
            "sub-millisecond samples advance instead of sharing a coarse clock tick");
        AdvanceZoomTransition(1090.0);
        Check(std::fabs(fixture.Value() - 42.5f) < .001f,
            "delayed worker updates preserve elapsed-time easing");
        AdvanceZoomTransition(1180.0);
        Check(fixture.Value() == 15.0f && !g_zoom.transitionActive,
            "transition finishes exactly at its configured duration after skipped updates");

        fixture.Value(15.0f);
        g_zoom.displayedValue = 15.0f;
        g_zoom.transitionStartValue = 15.0f;
        g_zoom.transitionTargetValue = 15.0005f;
        g_zoom.transitionStartedAt = 2000.0;
        g_zoom.transitionActive = true;
        AdvanceZoomTransition(2180.0);
        Check(fixture.Value() == g_zoom.displayedValue && fixture.Value() == 15.0005f,
            "small final steps still commit the exact target used by future CAS writes");

        fixture.Value(65.0f);
        g_zoom.displayedValue = 15.0005f;
        g_zoom.transitionStartValue = 15.0005f;
        g_zoom.transitionTargetValue = 20.0f;
        g_zoom.transitionActive = true;
        AdvanceZoomTransition(2090.0);
        Check(fixture.Value() == 65.0f && g_zoomRestorePending,
            "transition still preserves a setting changed by another writer");
        g_zoom = {};
        g_zoomRestorePending = false;
        g_zoomConfig.transitionDurationMs = originalDuration;
    }

    void CompleteScan()
    {
        Check(StartFovScan(), "scan starts");
        const ULONGLONG deadline = GetTickCount64() + 10'000;
        while (g_fovScan.result == FovScanResult::Running || g_fovScan.result == FovScanResult::Verifying)
        {
            Check(GetTickCount64() < deadline, "scan should complete promptly");
            AdvanceFovScan();
            Sleep(1);
        }
    }
}

#include "ScreenInputTests.h"

int main()
{
    try
    {
        g_gameModuleBase = 0x10000000; // Synthetic profile; no game addresses are accessed.
        g_supportedBuild = true;
        TestToolkitConfig();
        TestScreenInput();
        TestZoomTransitionInterpolation();
        TestDeferredZoomDiagnostics();
        TestZoomTransitionTiming();
        TestMessageHookOwnership();
        TestWorkerWaitMessages();
        TestOverlayVisibilityPositionFlags();
        TestZoomWheelTransitionRetargeting();
        {
            // Exercise the game's message path without installing any desktop
            // hook or injecting input into the user's applications.
            for (const UINT type : { WM_MOUSEWHEEL, WM_POINTERWHEEL })
            {
                InterlockedExchange(&g_rawWheelInputActive, 0);
                ClearZoomWheelInput();
                MSG message{};
                message.message = type;
                message.wParam = MAKEWPARAM(7, static_cast<WORD>(-WHEEL_DELTA));
                message.lParam = MAKELPARAM(100, 200);
                Check(!FilterZoomWheelMessage(message, false) && message.message == type &&
                    message.lParam == MAKELPARAM(100, 200) && g_pendingZoomWheel == 0,
                    "inactive Zoom/focus loss preserves wheel input");
                Check(FilterZoomWheelMessage(message, true) && message.message == WM_NULL &&
                    message.wParam == 0 && message.lParam == 0 && g_pendingZoomWheel == -WHEEL_DELTA,
                    "mouse and pointer wheel messages forward signed delta and suppress gameplay message");

                ClearZoomWheelInput();
                message.message = type;
                message.wParam = MAKEWPARAM(7, 30);
                Check(FilterZoomWheelMessage(message, true) && g_pendingZoomWheel == 30,
                    "high resolution wheel retains partial ticks");
                CaptureRawWheel(30);
                Check(g_pendingZoomWheel == 30, "message then Raw Input counts the first tick once");
                message.message = type;
                message.wParam = MAKEWPARAM(7, 30);
                Check(FilterZoomWheelMessage(message, true) && g_pendingZoomWheel == 30,
                    "Raw Input remains authoritative for subsequent messages");
                CaptureRawWheel(-30);
                Check(g_pendingZoomWheel == 0, "subsequent raw delta retains its sign");

                InterlockedExchange(&g_rawWheelInputActive, 0);
                ClearZoomWheelInput();
                message.message = type;
                message.wParam = MAKEWPARAM(7, 0);
                Check(!FilterZoomWheelMessage(message, true) && message.message == type && g_pendingZoomWheel == 0,
                    "zero wheel delta is left untouched");
                message.message = WM_MOUSEMOVE;
                message.wParam = MAKEWPARAM(7, WHEEL_DELTA);
                Check(!FilterZoomWheelMessage(message, true) && message.message == WM_MOUSEMOVE && g_pendingZoomWheel == 0,
                    "cursor motion is never captured by wheel filter");
            }
        }
        {
            std::array<unsigned char, 64> data{};
            const uintptr_t expected = 0x1122334455667788;
            std::memcpy(data.data(), &expected, 8);
            std::memcpy(data.data() + 56, &expected, 8);
            uintptr_t count = 0;
            Check(fov::FindOptions(data.data(), data.size(), 0x10000, expected,
                [&count](uintptr_t) { ++count; return true; }) && count == 2, "SIMD first and final slot");
            count = 0;
            const uintptr_t partial = 0xAABBCCDD55667788;
            std::memcpy(data.data(), &partial, 8);
            std::memcpy(data.data() + 56, &partial, 8);
            fov::FindOptions(data.data(), data.size(), 0x10000, expected,
                [&count](uintptr_t) { ++count; return true; });
            Check(count == 0, "both pointer halves must match");
            std::memcpy(data.data() + 5, &expected, 8);
            fov::FindOptions(data.data(), data.size(), 0x10003, expected,
                [&count](uintptr_t address) { Check(address == 0x10008, "absolute alignment"); ++count; return true; });
            Check(count == 1, "unaligned snapshot start");
        }
        {
            Fixture fixture;
            fov::Target verified;
            float values[5]{};
            Check(ReadVerifiedTarget(fixture.target.options, verified, values) &&
                fov::SameTarget(verified, fixture.target), "full object identity");
            CompleteScan();
            Check(g_fovScan.result == FovScanResult::Unique &&
                fov::SameTarget(g_fovScan.foundTarget, fixture.target), "unique full scan, including own scratch buffer");
            Check(fixture.Value() == 70, "discovery never writes FOV");
            Check(CompareExchangeFov(fixture.target.pattern + 8, 70, 10.0f), "zoom to 10");
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = 10.0f;
            Check(fixture.Value() == 10 && RestoreZoom() == FovRestoreResult::Restored && fixture.Value() == 70,
                "restore original FOV");
            fixture.Value(95);
            Check(!CompareExchangeFov(fixture.target.pattern + 8, 70, 10) && fixture.Value() == 95, "concurrent setting change");
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = 10.0f;
            Check(RestoreZoom() == FovRestoreResult::ValueChanged && fixture.Value() == 95, "external changes preserved");
            fixture.Value(10);
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = 10.0f;
            fixture.memory[1152] = 'x';
            Check(!ReadVerifiedTarget(fixture.target.options, verified, values), "wrong semantic key refused");
            Check(RestoreZoom() == FovRestoreResult::ValueChanged && fixture.Value() == 10, "reused object not overwritten");
            fixture.memory[1152] = 'g';
            fixture.Value(70);
            float bad = .00105f;
            std::memcpy(fixture.memory + 1056, &bad, 4);
            Check(!ReadVerifiedTarget(fixture.target.options, verified, values), "step tolerance is strict");
            bad = .001f;
            std::memcpy(fixture.memory + 1056, &bad, 4);
            g_supportedBuild = false;
            Check(!ReadVerifiedTarget(fixture.target.options, verified, values) && !StartFovScan(), "unverified build refused");
            g_supportedBuild = true;
            DWORD previous = 0;
            VirtualProtect(fixture.memory, 4096, PAGE_NOACCESS, &previous);
            Check(!ReadVerifiedTarget(fixture.target.options, verified, values), "inaccessible object refused");
            VirtualProtect(fixture.memory, 4096, previous, &previous);
            g_zoom = {};
            g_zoomRestorePending = false;
        }
        {
            Fixture first;
            Fixture second;
            CompleteScan();
            Check(g_fovScan.result == FovScanResult::Multiple && g_zoomReadiness == 3,
                "two valid FOV objects must refuse writes");
            Check(first.Value() == 70 && second.Value() == 70, "ambiguous scan never writes");
        }
        CompleteScan();
        Check(g_fovScan.result == FovScanResult::None, "missing target refused");
        g_fovScan.result = FovScanResult::Running;
        g_fovScan.startedAt = GetTickCount64() - kFovScanTimeoutMs - 1;
        AdvanceFovScan();
        Check(g_fovScan.result == FovScanResult::TimedOut && g_zoomReadiness == 3, "partial/timeout scan refused");
        Check(fov::MayActivate(true, true, false, false, false, false, true), "held key activates when ready");
        Check(!fov::MayActivate(true, false, false, false, false, false, true), "released key does not activate");
        Check(!fov::MayActivate(false, true, false, false, false, false, true), "focus loss blocks pending activation");
        Check(!fov::MayActivate(true, true, true, false, false, false, true), "Escape blocks pending activation");
        Check(!fov::MayActivate(true, true, false, true, false, false, true), "release required after refocus/Escape");
        Check(!fov::MayActivate(true, true, false, false, true, false, true), "restore pending blocks activation");
        Check(!fov::MayActivate(true, true, false, false, false, true, true), "stopping blocks pending activation");
        Check(!fov::MayActivate(true, true, false, false, false, false, false), "unverified result blocks activation");
        Check(fov::ShouldRestoreZoom(false, true, false),
            "active Zoom restores after key release even without a prior sampled key-down");
        Check(!fov::ShouldRestoreZoom(true, true, false), "held Zoom key does not restore");
        Check(!fov::ShouldRestoreZoom(false, false, false), "inactive Zoom does not restore");
        Check(!fov::ShouldRestoreZoom(false, true, true), "already restoring Zoom does not restart restoration");
        std::puts("FOV discovery tests passed: unique/multiple/missing/timeout, object identity, protected memory, CAS/restore, pending input and local wheel messages.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FOV discovery test failed: %s\n", error.what());
        return 1;
    }
}
