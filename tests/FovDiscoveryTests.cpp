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

int main()
{
    try
    {
        g_gameModuleBase = 0x10000000; // Synthetic profile; no game addresses are accessed.
        g_supportedBuild = true;
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
            Check(CompareExchangeFov(fixture.target.pattern + 8, 70, kDefaultZoomFov), "zoom to 10");
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = kDefaultZoomFov;
            Check(fixture.Value() == 10 && RestoreZoom() == FovRestoreResult::Restored && fixture.Value() == 70,
                "restore original FOV");
            fixture.Value(95);
            Check(!CompareExchangeFov(fixture.target.pattern + 8, 70, 10) && fixture.Value() == 95, "concurrent setting change");
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = kDefaultZoomFov;
            Check(RestoreZoom() == FovRestoreResult::ValueChanged && fixture.Value() == 95, "external changes preserved");
            fixture.Value(10);
            g_zoom = {fixture.target, 70, true};
            g_zoom.displayedValue = kDefaultZoomFov;
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
        Check(!fov::MayActivate(true, true, false, false, false, true, true), "F9 blocks pending activation");
        Check(!fov::MayActivate(true, true, false, false, false, false, false), "unverified result blocks activation");
        std::puts("FOV discovery tests passed: unique/multiple/missing/timeout, object identity, protected memory, CAS/restore, pending input and local wheel messages.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FOV discovery test failed: %s\n", error.what());
        return 1;
    }
}
