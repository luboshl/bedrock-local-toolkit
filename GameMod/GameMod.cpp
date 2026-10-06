#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <intrin.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <appmodel.h>
#include <tlhelp32.h>
#include <GameInput.h>
#include "vendor/minhook/include/MinHook.h"
#include "FovDiscovery.h"
#include "Nametag.h"
#include "AlwaysDay.h"
#include "FullBright.h"

namespace
{
    constexpr COLORREF kTransparentColor = RGB(255, 0, 255);
    constexpr COLORREF kPanelColor = RGB(20, 34, 30);
    constexpr wchar_t kWindowClassName[] = L"MinecraftClient.GameMod.LocalOverlay";
    constexpr wchar_t kExpectedGameModuleName[] = L"Minecraft.Windows.exe";
    constexpr float kDefaultZoomFov = 10.0f;
    constexpr float kZoomMinimumFov = 1.0f;
    constexpr float kZoomMaximumFov = 120.0f;
    constexpr float kZoomWheelStep = 5.0f;
    constexpr float kMouseSensitivityMinimum = 0.0f;
    constexpr float kMouseSensitivityMaximum = 100.0f;
    constexpr float kMouseSensitivityRuntimeMinimum = 0.0f;
    constexpr float kMouseSensitivityRuntimeMaximum = 1.0f;
    constexpr float kMouseSensitivityPercentScale = 0.01f;
    constexpr ULONGLONG kDefaultZoomTransitionDurationMs = 180;
    struct ZoomConfig
    {
        float fov = kDefaultZoomFov;
        ULONGLONG transitionDurationMs = kDefaultZoomTransitionDurationMs;
        float mouseSensitivity = 12.0f;
        int zoomKey = 'C';
        int nametagKey = VK_F7;
        int alwaysDayKey = VK_F6;
        int fullBrightKey = VK_F8;
        int indicatorKey = VK_F9;
        int exitKey = VK_F10;
    };
    ZoomConfig g_zoomConfig;
    constexpr float kFovMatchTolerance = fov::kTolerance;
    constexpr SIZE_T kFovScanChunkBytes = 4 * 1024 * 1024;
    constexpr SIZE_T kFovPatternBytes = sizeof(float) * 5;
    constexpr ULONGLONG kFovScanTimeoutMs = 15'000;
    constexpr ULONGLONG kFovRetryDelayMs = 2'000;
    constexpr ULONGLONG kFovVerifyDelayMs = 100;

    enum class FovScanResult
    {
        Idle,
        Running,
        Verifying,
        Unique,
        None,
        Multiple,
        TimedOut,
        ReadFailure,
        QueryFailure
    };

    enum class FovRestoreResult
    {
        NotActive,
        Restored,
        ValueChanged,
        Failed
    };

    struct FovZoomState
    {
        fov::Target target;
        float originalValue = 0.0f;
        bool active = false;
        float originalMinimumValue = 30.0f;
        float currentZoomValue = kDefaultZoomFov;
        float displayedValue = 0.0f;
        float transitionStartValue = 0.0f;
        float transitionTargetValue = 0.0f;
        ULONGLONG transitionStartedAt = 0;
        bool transitionActive = false;
        bool restoring = false;
    };

    struct ZoomSensitivityState
    {
        fov::Target target;
        float originalValue = 0.0f;
        bool active = false;
    };

    struct FovPointerConfig
    {
        std::wstring packageFullName;
        std::wstring moduleName;
        uintptr_t moduleRva = 0;
        std::vector<uintptr_t> offsets;
        uintptr_t targetOffset = 0;
        bool valid = false;
    };

    struct FovScanState
    {
        FovScanResult result = FovScanResult::Idle;
        uintptr_t cursor = 0;
        uintptr_t maxAddressExclusive = 0;
        fov::Target foundTarget;
        unsigned int matches = 0;
        ULONGLONG startedAt = 0;
        SIZE_T bytesScanned = 0;
        ULONGLONG finishedAt = 0;
        std::vector<unsigned char> buffer;
    };

    HMODULE g_module = nullptr;
    HWND g_overlayWindow = nullptr;
    HWND g_trackedGameWindow = nullptr;
    HHOOK g_zoomKeyboardHook = nullptr;
    volatile LONG g_zoomKeyDown = 0;
    volatile LONG g_zoomInputEpoch = 0;
    struct GameInputHook
    {
        DWORD threadId = 0;
        HHOOK handle = nullptr;
    };
    std::vector<GameInputHook> g_gameInputHooks;
    HBRUSH g_transparentBrush = nullptr;
    volatile LONG g_stopRequested = 0;
    volatile LONG g_indicatorVisible = 0;
    volatile LONG g_workerStarted = 0;
    volatile LONG g_pendingZoomWheel = 0;
    volatile LONG g_rawWheelInputActive = 0;
    volatile LONG g_wheelDedupeLock = 0;
    LONG g_zoomWheelRemainder = 0;
    volatile LONG g_lastLegacyWheelDelta = 0;
    volatile LONG64 g_lastLegacyWheelAt = 0;
    volatile LONG g_rawWheelEventLogged = 0;
    volatile LONG g_pointerWheelEventLogged = 0;
    volatile LONG g_messageWheelEventLogged = 0;
    // GameInput exposes cumulative relative mouse movement. Scaling the
    // cumulative coordinates makes the game's next-reading delta smaller.
    volatile LONG g_zoomMouseScalingActive = 0;
    volatile LONG g_zoomMouseScaleBasisPoints = 10'000;
    using GetRawInputDataFunction = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    using GetRawInputBufferFunction = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
    using GameInputCreateFunction = HRESULT(WINAPI*)(IGameInput**);
    using GameInputGetMouseStateFunction = bool(STDMETHODCALLTYPE*)(IGameInputReading*, GameInputMouseState*);
    using GameInputGetKeyStateFunction = uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, GameInputKeyState*);
    using GetKeyboardKeyStateFunction = SHORT(WINAPI*)(int);
    using GetKeyboardStateFunction = BOOL(WINAPI*)(PBYTE);
    struct GameInputMouseStateV2Compat
    {
        uint32_t buttons;
        uint32_t positions;
        int64_t positionX;
        int64_t positionY;
        int64_t absolutePositionX;
        int64_t absolutePositionY;
        int64_t wheelX;
        int64_t wheelY;
    };
    static_assert(sizeof(GameInputMouseStateV2Compat) == 0x38);
    static_assert(offsetof(GameInputMouseStateV2Compat, wheelY) == 0x30);
    struct MouseInputTrace
    {
        uintptr_t device = 0;
        LONG epoch = 0;
        bool capture = false;
        LONG scaling = 0;
        int64_t rawWheel = 0;
        int64_t returnedWheel = 0;
    };
    SRWLOCK g_mouseInputTraceLock = SRWLOCK_INIT;
    MouseInputTrace g_mouseInputTraces[64]{};
    unsigned int g_mouseInputTraceCount = 0;
    unsigned int g_mouseInputTraceLines = 0;
    struct MousePositionScaleState
    {
        bool initialized = false;
        int64_t lastInput = 0;
        long double filteredPosition = 0.0L;
    };
    struct PerDeviceMousePositionScaleState
    {
        uintptr_t device = 0;
        MousePositionScaleState x;
        MousePositionScaleState y;
    };
    using NativeMouseWheelFunction = void(__fastcall*)(float);
    using NativeMouseEventFunction = void(__fastcall*)(void*, int, int, int, int, int, int, bool);
    NativeMouseWheelFunction g_originalNativeMouseWheel = nullptr;
    NativeMouseEventFunction g_originalNativeMouseEvent = nullptr;
    void* g_nativeMouseWheelTarget = nullptr;
    void* g_nativeMouseEventTarget = nullptr;
    volatile LONG g_nativeMouseCallbacks = 0;
    constexpr unsigned char kNativeMouseEntries[2][16] = {
        { 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48,
            0x8D, 0x6C, 0x24, 0x60, 0x0F, 0x29, 0x75, 0xF0 },
        { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
            0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48 }
    };
    struct NativeMouseHookState
    {
        std::array<unsigned char, 16> replacement{};
        bool replacementVerified = false;
        bool disabled = false;
    };
    NativeMouseHookState g_nativeMouseHookStates[2]{};
    SRWLOCK g_mousePositionScaleLock = SRWLOCK_INIT;
    PerDeviceMousePositionScaleState g_gameInputMousePositionScale[64]{};
    using GameInputV2GetMouseStateFunction = bool(STDMETHODCALLTYPE*)(void*, GameInputMouseStateV2Compat*);
    using GameInputV2GetKeyStateFunction = uint32_t(STDMETHODCALLTYPE*)(void*, uint32_t, GameInputKeyState*);
    GetRawInputDataFunction g_originalGetRawInputData = nullptr;
    GetRawInputBufferFunction g_originalGetRawInputBuffer = nullptr;
    GetKeyboardKeyStateFunction g_originalGetAsyncKeyState = nullptr;
    GetKeyboardKeyStateFunction g_originalGetKeyState = nullptr;
    GetKeyboardStateFunction g_originalGetKeyboardState = nullptr;
    GameInputGetMouseStateFunction g_originalGameInputGetMouseState = nullptr;
    GameInputV2GetMouseStateFunction g_originalGameInputV2GetMouseState = nullptr;
    GameInputGetKeyStateFunction g_originalGameInputGetKeyState = nullptr;
    GameInputV2GetKeyStateFunction g_originalGameInputV2GetKeyState = nullptr;
    void* g_gameInputMouseStateTarget = nullptr;
    void* g_gameInputV2MouseStateTarget = nullptr;
    void* g_gameInputKeyStateTarget = nullptr;
    void* g_gameInputV2KeyStateTarget = nullptr;
    void* g_rawInputDataTarget = nullptr;
    void* g_rawInputBufferTarget = nullptr;
    void* g_getAsyncKeyStateTarget = nullptr;
    void* g_getKeyStateTarget = nullptr;
    void* g_getKeyboardStateTarget = nullptr;
    bool g_minHookInitializedByUs = false;
    struct ImportAddressPatch
    {
        uintptr_t* slot = nullptr;
        uintptr_t original = 0;
    };
    ImportAddressPatch g_inputImportPatches[256];
    unsigned int g_inputImportPatchCount = 0;
    bool g_zoomRestorePending = false;
    FovZoomState g_zoom;
    ZoomSensitivityState g_zoomSensitivity;
    FovPointerConfig g_fovPointerConfig;
    FovScanState g_fovScan;
    uintptr_t g_gameModuleBase = 0;
    bool g_supportedBuild = false;
    volatile LONG g_zoomReadiness = 0; // 0 starting, 1 searching, 2 ready, 3 refused
    std::wstring g_statusLogPath;
    std::wstring g_nametagLogPath;
    std::wstring g_alwaysDayLogPath;
    std::wstring g_fullBrightLogPath;
    const wchar_t* g_overlayMessage = L"Local world: Zoom | Full Bright | Always day | Nametag | exit (see bedrock-toolkit.ini)";
    COLORREF g_overlayTextColor = RGB(135, 255, 170);

    void SetOverlayMessage(const wchar_t* message, COLORREF color)
    {
        if (g_overlayMessage != message)
        {
            g_overlayMessage = message;
            g_overlayTextColor = color;
            if (g_overlayWindow != nullptr)
            {
                InvalidateRect(g_overlayWindow, nullptr, TRUE);
            }
        }
    }

    BOOL CALLBACK FindGameWindow(HWND window, LPARAM result)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == GetCurrentProcessId() && IsWindowVisible(window))
        {
            *reinterpret_cast<HWND*>(result) = window;
            return FALSE;
        }

        return TRUE;
    }

    HWND GetGameWindow()
    {
        HWND window = nullptr;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
        return window;
    }

    bool IsGameForeground(HWND gameWindow)
    {
        const HWND foreground = GetForegroundWindow();
        if (foreground == nullptr || gameWindow == nullptr)
        {
            return false;
        }

        DWORD foregroundProcessId = 0;
        GetWindowThreadProcessId(foreground, &foregroundProcessId);
        if (foregroundProcessId == GetCurrentProcessId())
        {
            return true;
        }

        const HWND foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
        return foregroundRoot == gameWindow ||
            IsChild(foregroundRoot, gameWindow) ||
            IsChild(gameWindow, foregroundRoot);
    }

    bool IsGameActive(HWND gameWindow)
    {
        return gameWindow != nullptr && IsWindowVisible(gameWindow) &&
            !IsIconic(gameWindow) && IsGameForeground(gameWindow);
    }

    bool IsZoomKeyDown()
    {
        return InterlockedCompareExchange(&g_zoomKeyDown, 0, 0) != 0 ||
            (g_zoomKeyboardHook == nullptr && (GetAsyncKeyState(g_zoomConfig.zoomKey) & 0x8000) != 0);
    }

    bool IsFovPattern(const float values[5])
    {
        return fov::IsPattern(values);
    }

    bool ReadPrivateMemory(uintptr_t address, void* output, SIZE_T size)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (address == 0 || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
            (memory.Protect & 0xFF) != PAGE_READWRITE ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (address < regionBase || address - regionBase > memory.RegionSize ||
            memory.RegionSize - (address - regionBase) < size)
        {
            return false;
        }

        SIZE_T bytesRead = 0;
        return ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address), output, size,
            &bytesRead) != FALSE && bytesRead == size;
    }

    bool ReadFovPattern(uintptr_t patternBase, float values[5])
    {
        return ReadPrivateMemory(patternBase, values, kFovPatternBytes);
    }

    bool MatchOptionString(uintptr_t address, const char* expected, size_t length)
    {
        // MSVC std::string layout used by this exact build, including SSO.
        unsigned char text[32]{};
        if (!ReadPrivateMemory(address, text, sizeof(text))) return false;
        uintptr_t size = 0, capacity = 0;
        std::memcpy(&size, text + 16, 8);
        std::memcpy(&capacity, text + 24, 8);
        if (size != length || capacity < size || capacity > 4096) return false;
        char external[64]{};
        if (capacity >= 16)
        {
            uintptr_t pointer = 0;
            std::memcpy(&pointer, text, 8);
            if (length + 1 > sizeof(external) || !ReadPrivateMemory(pointer, external, length + 1)) return false;
            return std::memcmp(external, expected, length + 1) == 0;
        }
        return length < 16 && std::memcmp(text, expected, length + 1) == 0;
    }

    bool ReadVerifiedTarget(uintptr_t options, fov::Target& target, float values[5])
    {
        if (!g_supportedBuild || options == 0 || (options & 7) != 0 ||
            options > (std::numeric_limits<uintptr_t>::max)() - fov::kFovOptionOffset) return false;
        uintptr_t vtable = 0, wrapper = 0;
        if (!ReadPrivateMemory(options, &vtable, 8) || vtable != g_gameModuleBase + fov::kOptionsVtableRva ||
            !ReadPrivateMemory(options + fov::kFovOptionOffset, &wrapper, 8) ||
            wrapper == 0 || (wrapper & 7) != 0 ||
            wrapper > (std::numeric_limits<uintptr_t>::max)() - fov::kPatternOffset) return false;
        uintptr_t header[2]{};
        if (!ReadPrivateMemory(wrapper, header, sizeof(header)) ||
            header[0] != g_gameModuleBase + fov::kValueVtableRva || header[1] == 0 ||
            (header[1] & 7) != 0 || header[1] > (std::numeric_limits<uintptr_t>::max)() - 0x200 ||
            !MatchOptionString(header[1] + fov::kKeyOffset, "gfx_field_of_view", 17) ||
            !MatchOptionString(header[1] + fov::kDisplayNameOffset, "fieldOfView", 11) ||
            !MatchOptionString(header[1] + fov::kCaptionOffset, "options.fov", 11) ||
            !ReadFovPattern(wrapper + fov::kPatternOffset, values) || !IsFovPattern(values)) return false;
        target = {options, wrapper + fov::kPatternOffset, header[1]};
        return true;
    }

    bool ReadVerifiedMouseSensitivity(const fov::Target& expectedTarget, float& value)
    {
        if (!g_supportedBuild || expectedTarget.options == 0 ||
            expectedTarget.options > (std::numeric_limits<uintptr_t>::max)() - fov::kOptionsSearchBytes)
            return false;

        // Resolve the named mouse sensitivity slider from the same verified
        // Options object. A numeric value at an unverified offset is not enough.
        fov::Target currentTarget;
        float fovValues[5]{};
        if (!ReadVerifiedTarget(expectedTarget.options, currentTarget, fovValues) ||
            !fov::SameTarget(currentTarget, expectedTarget))
            return false;

        unsigned int matches = 0;
        float foundValue = 0.0f;
        for (size_t offset = 0; offset + sizeof(uintptr_t) <= fov::kOptionsSearchBytes;
            offset += sizeof(uintptr_t))
        {
            uintptr_t wrapper = 0;
            if (!ReadPrivateMemory(expectedTarget.options + offset, &wrapper, sizeof(wrapper))) return false;
            if (wrapper == 0 || (wrapper & 7) != 0 ||
                wrapper > (std::numeric_limits<uintptr_t>::max)() - fov::kPatternOffset) continue;

            uintptr_t header[2]{};
            if (!ReadPrivateMemory(wrapper, header, sizeof(header)) ||
                header[0] != g_gameModuleBase + fov::kMouseSensitivityVtableRva || header[1] == 0 ||
                (header[1] & 7) != 0 || header[1] > (std::numeric_limits<uintptr_t>::max)() - 0x200 ||
                !MatchOptionString(header[1] + fov::kKeyOffset, "ctrl_sensitivity2", sizeof("ctrl_sensitivity2") - 1) ||
                !MatchOptionString(header[1] + fov::kDisplayNameOffset, "sensitivity", sizeof("sensitivity") - 1) ||
                !MatchOptionString(header[1] + fov::kCaptionOffset, "options.sensitivity", sizeof("options.sensitivity") - 1))
                continue;

            float values[4]{};
            if (!ReadPrivateMemory(wrapper + fov::kPatternOffset, values, sizeof(values)) ||
                !std::isfinite(values[0]) || !std::isfinite(values[1]) || !std::isfinite(values[2]) ||
                std::fabs(values[0] - 0.0f) >= kFovMatchTolerance ||
                std::fabs(values[1] - 1.0f) >= kFovMatchTolerance ||
                values[2] < kMouseSensitivityRuntimeMinimum || values[2] > kMouseSensitivityRuntimeMaximum)
                continue;

            ++matches;
            foundValue = values[2];
        }

        if (matches != 1) return false;
        value = foundValue;
        return true;
    }

    void LogZoomStatus(const char* event)
    {
        if (g_statusLogPath.empty()) return;
        char line[512]{};
        const int length = sprintf_s(line,
            "pid=%lu event=%s result=%d elapsed_ms=%llu bytes=%llu matches=%u options=0x%llX pattern=0x%llX owner=0x%llX\r\n",
            GetCurrentProcessId(), event, static_cast<int>(g_fovScan.result),
            GetTickCount64() - g_fovScan.startedAt, static_cast<unsigned long long>(g_fovScan.bytesScanned),
            g_fovScan.matches, static_cast<unsigned long long>(g_fovScan.foundTarget.options),
            static_cast<unsigned long long>(g_fovScan.foundTarget.pattern),
            static_cast<unsigned long long>(g_fovScan.foundTarget.owner));
        const HANDLE file = CreateFileW(g_statusLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
            CloseHandle(file);
        }
    }

    void LogNametagStatus(const char* event)
    {
        if (g_nametagLogPath.empty()) return;
        char line[512]{};
        const int length = sprintf_s(line,
            "pid=%lu event=%s status=%s ready=%ld enabled=%ld own_checks=%lld rear=%lld front=%lld depth=%lld\r\n",
            GetCurrentProcessId(), event, nametag::status,
            InterlockedCompareExchange(&nametag::readiness, 0, 0),
            InterlockedCompareExchange(&nametag::enabled, 0, 0),
            InterlockedCompareExchange64(&nametag::ownChecks, 0, 0),
            InterlockedCompareExchange64(&nametag::shownRear, 0, 0),
            InterlockedCompareExchange64(&nametag::shownFront, 0, 0),
            InterlockedCompareExchange64(&nametag::depthSelections, 0, 0));
        const HANDLE file = CreateFileW(g_nametagLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
            CloseHandle(file);
        }
    }

    // Atomic value comparison prevents overwriting a setting changed between
    // verification and the write. SEH handles an allocation disappearing then.
    bool CompareExchangeFov(uintptr_t address, float expected, float replacement)
    {
        LONG expectedBits = 0, replacementBits = 0;
        std::memcpy(&expectedBits, &expected, sizeof(expected));
        std::memcpy(&replacementBits, &replacement, sizeof(replacement));
        __try
        {
            return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), replacementBits, expectedBits) == expectedBits;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    enum class SensitivityRestoreResult
    {
        NotActive,
        Restored,
        ValueChanged,
        Failed
    };

    SensitivityRestoreResult RestoreZoomSensitivity()
    {
        if (!g_zoomSensitivity.active) return SensitivityRestoreResult::NotActive;
        InterlockedExchange(&g_zoomMouseScalingActive, 0);
        InterlockedExchange(&g_zoomMouseScaleBasisPoints, 10'000);
        g_zoomSensitivity.active = false;
        LogZoomStatus("mouse-sensitivity-scaling-restored");
        return SensitivityRestoreResult::Restored;
    }

    FovRestoreResult CompleteZoomRestore(FovRestoreResult fovResult)
    {
        const SensitivityRestoreResult sensitivityResult = RestoreZoomSensitivity();
        if (sensitivityResult == SensitivityRestoreResult::Failed)
        {
            g_zoomRestorePending = true;
            return FovRestoreResult::Failed;
        }

        g_zoom.active = false;
        g_zoomRestorePending = false;
        g_zoom.transitionActive = false;
        g_zoom.restoring = false;
        if (fovResult == FovRestoreResult::ValueChanged ||
            sensitivityResult == SensitivityRestoreResult::ValueChanged)
            return FovRestoreResult::ValueChanged;

        LogZoomStatus("restored");
        return FovRestoreResult::Restored;
    }

    bool ParseUnsignedValue(const std::wstring& text, uintptr_t& value)
    {
        if (text.empty() || text[0] < L'0' || text[0] > L'9')
        {
            return false;
        }

        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 0);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' ||
            parsed > (std::numeric_limits<uintptr_t>::max)())
        {
            return false;
        }

        value = static_cast<uintptr_t>(parsed);
        return true;
    }

    std::wstring TrimWhitespace(const std::wstring& text)
    {
        const size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return {};
        }

        const size_t last = text.find_last_not_of(L" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    bool ReadConfigValue(const wchar_t* key, std::wstring& value, const std::wstring& configPath)
    {
        wchar_t buffer[1024]{};
        const DWORD length = GetPrivateProfileStringW(
            L"PointerChain", key, L"", buffer, static_cast<DWORD>(std::size(buffer)), configPath.c_str());
        if (length == 0 || length >= std::size(buffer) - 1)
        {
            return false;
        }

        value.assign(buffer, length);
        return true;
    }

    bool ReadToolkitConfigValue(const wchar_t* section, const wchar_t* key, std::wstring& value, const std::wstring& configPath)
    {
        wchar_t buffer[128]{};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), configPath.c_str());
        if (length == 0 || length >= std::size(buffer) - 1) return false;
        value.assign(buffer, length);
        return true;
    }

    bool ParseVirtualKey(std::wstring value, int& key)
    {
        value = TrimWhitespace(value);
        for (wchar_t& character : value) character = static_cast<wchar_t>(std::towupper(character));
        if (value.size() >= 2 && value[0] == L'F')
        {
            wchar_t* end = nullptr;
            const long number = std::wcstol(value.c_str() + 1, &end, 10);
            if (end != value.c_str() + 1 && *end == L'\0' && number >= 1 && number <= 24)
            {
                key = VK_F1 + static_cast<int>(number) - 1;
                return true;
            }
        }
        if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') ||
            (value[0] >= L'0' && value[0] <= L'9')))
        {
            key = static_cast<int>(value[0]);
            return true;
        }
        if (value == L"SPACE") key = VK_SPACE;
        else if (value == L"TAB") key = VK_TAB;
        else if (value == L"INSERT") key = VK_INSERT;
        else if (value == L"HOME") key = VK_HOME;
        else if (value == L"END") key = VK_END;
        else if (value == L"PAGEUP") key = VK_PRIOR;
        else if (value == L"PAGEDOWN") key = VK_NEXT;
        else if (value == L"DELETE") key = VK_DELETE;
        else if (value == L"BACKSPACE") key = VK_BACK;
        else return false;
        return true;
    }

    void LogAlwaysDayStatus(const char* event)
    {
        if (g_alwaysDayLogPath.empty()) return;
        const HANDLE file = CreateFileW(g_alwaysDayLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        for (size_t i = 0; i < alwaysday::kCallerCount; ++i)
        {
            char line[256]{};
            const int length = sprintf_s(line,
                "pid=%lu event=%s status=%s ready=%ld enabled=%ld profile=3 source=%s return_rva=0x%llX noon_overrides=%lld\r\n",
                GetCurrentProcessId(), event, alwaysday::status,
                InterlockedCompareExchange(&alwaysday::readiness, 0, 0),
                InterlockedCompareExchange(&alwaysday::enabled, 0, 0),
                alwaysday::kRenderCallers[i].returnRva == alwaysday::kBrightnessReturnRva ? "light-image" : "render-time",
                static_cast<unsigned long long>(alwaysday::kRenderCallers[i].returnRva), alwaysday::OverrideCount(i));
            DWORD written = 0;
            if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        }
        char line[256]{};
        const int length = sprintf_s(line,
            "pid=%lu event=%s status=%s profile=3 stars_overrides=%lld cloud_overrides=%lld\r\n",
            GetCurrentProcessId(), event, alwaysday::status,
            alwaysday::StarsOverrideCount(), alwaysday::CloudOverrideCount());
        DWORD written = 0;
        if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }

    void LogFullBrightStatus(const char* event)
    {
        if (g_fullBrightLogPath.empty()) return;
        const HANDLE file = CreateFileW(g_fullBrightLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        char line[256]{};
        const int length = sprintf_s(line,
            "pid=%lu event=%s status=%s ready=%ld enabled=%ld profile=1 source=light-image brightness_overrides=%lld\r\n",
            GetCurrentProcessId(), event, fullbright::status,
            InterlockedCompareExchange(&fullbright::readiness, 0, 0),
            InterlockedCompareExchange(&fullbright::enabled, 0, 0), fullbright::OverrideCount());
        DWORD written = 0;
        if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }

    bool LoadZoomConfig()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (length == 0 || length >= std::size(modulePath)) return false;
        std::wstring configPath(modulePath, length);
        const size_t separator = configPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return false;
        configPath.resize(separator + 1);
        configPath += L"bedrock-toolkit.ini";

        ZoomConfig loaded;
        std::wstring value;
        if (ReadToolkitConfigValue(L"Zoom", L"Fov", value, configPath))
        {
            wchar_t* end = nullptr;
            value = TrimWhitespace(value);
            const float parsed = std::wcstof(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < kZoomMinimumFov || parsed > kZoomMaximumFov) return false;
            loaded.fov = parsed;
        }
        if (ReadToolkitConfigValue(L"Zoom", L"TransitionDurationMs", value, configPath))
        {
            value = TrimWhitespace(value);
            wchar_t* end = nullptr;
            errno = 0;
            const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
            if (errno == ERANGE || end == value.c_str() || *end != L'\0' || parsed < 1 || parsed > 10'000)
                return false;
            loaded.transitionDurationMs = parsed;
        }
        if (ReadToolkitConfigValue(L"Zoom", L"MouseSensitivity", value, configPath))
        {
            value = TrimWhitespace(value);
            wchar_t* end = nullptr;
            const float parsed = std::wcstof(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < kMouseSensitivityMinimum || parsed > kMouseSensitivityMaximum)
                return false;
            loaded.mouseSensitivity = parsed;
        }
        const auto readKey = [&configPath](const wchar_t* name, int& target)
        {
            std::wstring keyName;
            return !ReadToolkitConfigValue(L"Shortcuts", name, keyName, configPath) || ParseVirtualKey(keyName, target);
        };
        if (!readKey(L"Zoom", loaded.zoomKey) || !readKey(L"Nametag", loaded.nametagKey) ||
            !readKey(L"AlwaysDay", loaded.alwaysDayKey) || !readKey(L"FullBright", loaded.fullBrightKey) ||
            !readKey(L"StatusIndicator", loaded.indicatorKey) || !readKey(L"SafeDetach", loaded.exitKey)) return false;
        const int keys[] = {loaded.zoomKey, loaded.nametagKey, loaded.alwaysDayKey, loaded.fullBrightKey,
            loaded.indicatorKey, loaded.exitKey};
        for (size_t i = 0; i < std::size(keys); ++i)
            for (size_t j = i + 1; j < std::size(keys); ++j)
                if (keys[i] == keys[j]) return false;
        g_zoomConfig = loaded;
        return true;
    }

    bool LoadFovPointerConfig()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD modulePathLength = GetModuleFileNameW(g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (modulePathLength == 0 || modulePathLength >= std::size(modulePath))
        {
            return false;
        }

        std::wstring configPath(modulePath, modulePathLength);
        const size_t separator = configPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return false;
        }
        configPath.resize(separator + 1);
        configPath += L"zoom-pointer.ini";

        std::wstring packageFullName;
        std::wstring moduleName;
        std::wstring moduleRvaText;
        std::wstring offsetsText;
        std::wstring targetOffsetText;
        if (!ReadConfigValue(L"PackageFullName", packageFullName, configPath) ||
            !ReadConfigValue(L"ModuleName", moduleName, configPath) ||
            !ReadConfigValue(L"ModuleRva", moduleRvaText, configPath) ||
            !ReadConfigValue(L"PointerOffsets", offsetsText, configPath) ||
            !ReadConfigValue(L"TargetOffset", targetOffsetText, configPath))
        {
            return false;
        }
        if (moduleName != kExpectedGameModuleName)
        {
            return false;
        }

        uintptr_t moduleRva = 0;
        if (!ParseUnsignedValue(TrimWhitespace(moduleRvaText), moduleRva) || moduleRva == 0)
        {
            return false;
        }

        uintptr_t targetOffset = 0;
        if (!ParseUnsignedValue(TrimWhitespace(targetOffsetText), targetOffset) || targetOffset > 0x1000)
        {
            return false;
        }

        std::vector<uintptr_t> offsets;
        size_t tokenStart = 0;
        while (tokenStart <= offsetsText.size())
        {
            const size_t comma = offsetsText.find(L',', tokenStart);
            const size_t tokenEnd = comma == std::wstring::npos ? offsetsText.size() : comma;
            const std::wstring token = TrimWhitespace(offsetsText.substr(tokenStart, tokenEnd - tokenStart));
            uintptr_t offset = 0;
            if (!ParseUnsignedValue(token, offset) || offset > 0x1000000 || offsets.size() >= 8)
            {
                return false;
            }
            offsets.push_back(offset);

            if (comma == std::wstring::npos)
            {
                break;
            }
            tokenStart = comma + 1;
        }

        if (offsets.empty())
        {
            return false;
        }

        g_fovPointerConfig.packageFullName = std::move(packageFullName);
        g_fovPointerConfig.moduleName = std::move(moduleName);
        g_fovPointerConfig.moduleRva = moduleRva;
        g_fovPointerConfig.offsets = std::move(offsets);
        g_fovPointerConfig.targetOffset = targetOffset;
        g_fovPointerConfig.valid = true;
        return true;
    }

    bool ReadCurrentPackageFullName(std::wstring& packageFullName)
    {
        UINT32 bufferLength = 0;
        const LONG firstResult = GetPackageFullName(GetCurrentProcess(), &bufferLength, nullptr);
        if (firstResult != ERROR_INSUFFICIENT_BUFFER || bufferLength == 0)
        {
            return false;
        }

        std::vector<wchar_t> buffer(bufferLength);
        const LONG result = GetPackageFullName(GetCurrentProcess(), &bufferLength, buffer.data());
        if (result != ERROR_SUCCESS)
        {
            return false;
        }

        packageFullName.assign(buffer.data());
        return true;
    }

    bool ReadPointer(uintptr_t address, uintptr_t& value)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const DWORD protection = memory.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (!readable || address < regionBase || address - regionBase > memory.RegionSize ||
            memory.RegionSize - (address - regionBase) < sizeof(value))
        {
            return false;
        }

        SIZE_T bytesRead = 0;
        return ReadProcessMemory(
            GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value), &bytesRead) != FALSE &&
            bytesRead == sizeof(value);
    }

    bool InitializeFovBuild()
    {
        std::wstring package;
        if (!ReadCurrentPackageFullName(package) || package != fov::kPackage) return false;
        const HMODULE module = GetModuleHandleW(kExpectedGameModuleName);
        if (module == nullptr) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<uintptr_t>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->FileHeader.TimeDateStamp != fov::kImageTimestamp || nt->OptionalHeader.SizeOfImage != fov::kImageSize) return false;
        g_gameModuleBase = reinterpret_cast<uintptr_t>(module);
        for (const uintptr_t rva : {fov::kOptionsVtableRva, fov::kValueVtableRva})
        {
            uintptr_t method = 0;
            MEMORY_BASIC_INFORMATION memory{};
            if (!ReadPointer(g_gameModuleBase + rva, method) ||
                VirtualQuery(reinterpret_cast<const void*>(method), &memory, sizeof(memory)) == 0 ||
                memory.Type != MEM_IMAGE || memory.AllocationBase != module ||
                (memory.Protect & 0xFF) != PAGE_EXECUTE_READ) return false;
        }
        // Verify both consumers and their exact GameInput call sites before
        // installing any hooks. These are local mouse-input paths only.
        const auto matchesCode = [module](uintptr_t rva, const auto& expected)
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(module) + rva;
            MEMORY_BASIC_INFORMATION memory{};
            unsigned char actual[32]{};
            SIZE_T read = 0;
            return sizeof(expected) <= sizeof(actual) &&
                VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != 0 &&
                memory.Type == MEM_IMAGE && memory.AllocationBase == module &&
                (memory.Protect & 0xFF) == PAGE_EXECUTE_READ &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                    actual, sizeof(expected), &read) && read == sizeof(expected) &&
                std::memcmp(actual, expected, sizeof(expected)) == 0;
        };
        constexpr unsigned char wheelCall[] = { 0xE8, 0x57, 0x47, 0x24, 0x00 };
        constexpr unsigned char eventCall[] = { 0xE8, 0x90, 0x4C, 0x2D, 0x00 };
        constexpr unsigned char wheelDelta[] = { 0x8B, 0x9D, 0x20, 0x01, 0x00, 0x00,
            0x41, 0x2B, 0x5F, 0x40, 0x66, 0x85, 0xDB };
        if (!matchesCode(0x2D1560, kNativeMouseEntries[0]) || !matchesCode(0x361AD0, kNativeMouseEntries[1]) ||
            !matchesCode(0x8CE04, wheelCall) || !matchesCode(0x8CE3B, eventCall) ||
            !matchesCode(0x8CD5F, wheelDelta)) return false;
        g_supportedBuild = true;
        return true;
    }

    enum class FovResolveResult
    {
        Resolved,
        ConfigUnavailable,
        VersionMismatch,
        ModuleUnavailable,
        InvalidChain,
        PatternMismatch
    };

    FovResolveResult ResolveFovTarget(fov::Target& target)
    {
        if (!g_fovPointerConfig.valid)
        {
            return FovResolveResult::ConfigUnavailable;
        }

        std::wstring currentPackage;
        if (!ReadCurrentPackageFullName(currentPackage) || currentPackage != g_fovPointerConfig.packageFullName)
        {
            return FovResolveResult::VersionMismatch;
        }

        HMODULE gameModule = GetModuleHandleW(g_fovPointerConfig.moduleName.c_str());
        if (gameModule == nullptr)
        {
            return FovResolveResult::ModuleUnavailable;
        }

        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(gameModule);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameModule);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
        {
            return FovResolveResult::InvalidChain;
        }

        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            g_fovPointerConfig.moduleRva > ntHeaders->OptionalHeader.SizeOfImage ||
            ntHeaders->OptionalHeader.SizeOfImage - g_fovPointerConfig.moduleRva < sizeof(uintptr_t))
        {
            return FovResolveResult::InvalidChain;
        }

        uintptr_t cursor = moduleBase + g_fovPointerConfig.moduleRva;
        MEMORY_BASIC_INFORMATION rootMemory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &rootMemory, sizeof(rootMemory)) == 0 ||
            rootMemory.State != MEM_COMMIT || rootMemory.Type != MEM_IMAGE || rootMemory.AllocationBase != gameModule)
        {
            return FovResolveResult::InvalidChain;
        }

        uintptr_t finalParent = 0;
        for (const uintptr_t offset : g_fovPointerConfig.offsets)
        {
            if (cursor > (std::numeric_limits<uintptr_t>::max)() - offset)
            {
                return FovResolveResult::InvalidChain;
            }

            uintptr_t next = 0;
            if (!ReadPointer(cursor + offset, next) || next == 0 || (next & (alignof(uintptr_t) - 1)) != 0)
            {
                return FovResolveResult::InvalidChain;
            }
            finalParent = cursor;
            cursor = next;
        }

        if (cursor > (std::numeric_limits<uintptr_t>::max)() - g_fovPointerConfig.targetOffset)
        {
            return FovResolveResult::InvalidChain;
        }

        float values[5]{};
        if (g_fovPointerConfig.offsets.back() != fov::kFovOptionOffset ||
            g_fovPointerConfig.targetOffset != fov::kPatternOffset ||
            !ReadVerifiedTarget(finalParent, target, values) ||
            target.pattern != cursor + g_fovPointerConfig.targetOffset)
        {
            return FovResolveResult::PatternMismatch;
        }

        return FovResolveResult::Resolved;
    }

    FovRestoreResult RestoreZoom()
    {
        if (!g_zoom.active)
        {
            g_zoomRestorePending = false;
            g_zoom.transitionActive = false;
            g_zoom.restoring = false;
            return FovRestoreResult::NotActive;
        }

        float values[5]{};
        if (!ReadFovPattern(g_zoom.target.pattern, values))
        {
            g_zoomRestorePending = true;
            return FovRestoreResult::Failed;
        }

        fov::Target current;
        if (!ReadVerifiedTarget(g_zoom.target.options, current, values) || !fov::SameTarget(current, g_zoom.target))
        {
            return CompleteZoomRestore(FovRestoreResult::ValueChanged);
        }

        const uintptr_t minimumAddress = g_zoom.target.pattern;
        const uintptr_t valueAddress = minimumAddress + sizeof(float) * 2;
        const bool valueIsZoomed = std::fabs(values[2] - g_zoom.displayedValue) < kFovMatchTolerance;
        const bool valueIsOriginal = std::fabs(values[2] - g_zoom.originalValue) < kFovMatchTolerance;
        if (valueIsZoomed)
        {
            if (!CompareExchangeFov(valueAddress, g_zoom.displayedValue, g_zoom.originalValue))
            {
                g_zoomRestorePending = true;
                return FovRestoreResult::Failed;
            }
        }
        else if (!valueIsOriginal)
        {
            // Preserve a value changed by the game or the user while Zoom was active,
            // but still put back the temporary slider floor we own.
            if (std::fabs(values[0] - kZoomMinimumFov) < kFovMatchTolerance &&
                !CompareExchangeFov(minimumAddress, kZoomMinimumFov, g_zoom.originalMinimumValue))
            {
                g_zoomRestorePending = true;
                return FovRestoreResult::Failed;
            }
            return CompleteZoomRestore(FovRestoreResult::ValueChanged);
        }

        if (std::fabs(values[0] - kZoomMinimumFov) < kFovMatchTolerance &&
            !CompareExchangeFov(minimumAddress, kZoomMinimumFov, g_zoom.originalMinimumValue))
        {
            g_zoomRestorePending = true;
            return FovRestoreResult::Failed;
        }

        float verify[5]{};
        if (ReadVerifiedTarget(g_zoom.target.options, current, verify) && fov::SameTarget(current, g_zoom.target) &&
            std::fabs(verify[0] - g_zoom.originalMinimumValue) < kFovMatchTolerance &&
            std::fabs(verify[2] - g_zoom.originalValue) < kFovMatchTolerance)
        {
            return CompleteZoomRestore(FovRestoreResult::Restored);
        }

        g_zoomRestorePending = true;
        return FovRestoreResult::Failed;
    }

    bool StartFovScan();

    void ApplyZoomAtTarget(HWND gameWindow, const fov::Target& target)
    {
        if (g_zoomKeyboardHook == nullptr || g_nativeMouseWheelTarget == nullptr ||
            g_nativeMouseEventTarget == nullptr || !g_nativeMouseHookStates[0].replacementVerified ||
            !g_nativeMouseHookStates[1].replacementVerified)
        {
            SetOverlayMessage(L"Zoom unavailable; input capture could not be installed", RGB(255, 170, 110));
            return;
        }
        if (!IsGameActive(gameWindow))
        {
            SetOverlayMessage(L"Minecraft must be active; no memory changed", RGB(255, 170, 110));
            return;
        }

        fov::Target current;
        float values[5]{};
        if (g_fovScan.result != FovScanResult::Unique || !fov::SameTarget(target, g_fovScan.foundTarget) ||
            !ReadVerifiedTarget(target.options, current, values) || !fov::SameTarget(current, target))
        {
            InterlockedExchange(&g_zoomReadiness, 1);
            g_fovScan.result = FovScanResult::Idle;
            SetOverlayMessage(L"FOV target changed; no memory changed", RGB(255, 170, 110));
            return;
        }
        if (std::fabs(values[0] - kZoomMinimumFov) < kFovMatchTolerance &&
            std::fabs(values[2] - g_zoomConfig.fov) < kFovMatchTolerance)
        {
            SetOverlayMessage(L"FOV already matches the configured Zoom level", RGB(255, 220, 120));
            return;
        }

        // A direct write below the configured 30-degree floor did not change
        // the rendered view. Lower both compare-and-swap checked fields while
        // held so the game can accept the requested value below that floor.
        if (std::fabs(values[0] - 30.0f) >= kFovMatchTolerance)
        {
            SetOverlayMessage(L"FOV minimum changed; Zoom refused", RGB(255, 170, 110));
            return;
        }

        if (!IsGameActive(gameWindow) || !IsZoomKeyDown() ||
            (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
        {
            SetOverlayMessage(L"Minecraft lost focus; no memory changed", RGB(255, 170, 110));
            return;
        }

        float sensitivityValue = 0.0f;
        bool sensitivityApplied = ReadVerifiedMouseSensitivity(target, sensitivityValue);
        g_zoomSensitivity = {};
        if (!sensitivityApplied)
        {
            LogZoomStatus("mouse-sensitivity-target-unavailable-zoom-without-change");
        }

        g_zoom.target = target;
        g_zoom.originalMinimumValue = values[0];
        g_zoom.originalValue = values[2];
        g_zoom.currentZoomValue = g_zoomConfig.fov;
        g_zoom.displayedValue = values[2];
        g_zoom.transitionStartValue = values[2];
        g_zoom.transitionTargetValue = g_zoomConfig.fov;
        g_zoom.transitionStartedAt = GetTickCount64();
        g_zoom.transitionActive = true;
        g_zoom.restoring = false;
        g_zoom.active = true;
        g_zoomRestorePending = true;

        if (sensitivityApplied)
        {
            g_zoomSensitivity.target = target;
            g_zoomSensitivity.originalValue = sensitivityValue;
            g_zoomSensitivity.active = true;
            if (sensitivityValue > kFovMatchTolerance)
            {
                const double scale = static_cast<double>(g_zoomConfig.mouseSensitivity) /
                    (static_cast<double>(sensitivityValue) / kMouseSensitivityPercentScale);
                const LONG scaleBasisPoints = static_cast<LONG>(std::clamp(std::llround(scale * 10'000.0),
                    0LL, 100'000LL));
                InterlockedExchange(&g_zoomMouseScaleBasisPoints, scaleBasisPoints);
                InterlockedExchange(&g_zoomMouseScalingActive, 1);
                LogZoomStatus("mouse-sensitivity-scaling-applied");
            }
            else
            {
                sensitivityApplied = false;
                g_zoomSensitivity.active = false;
                LogZoomStatus("mouse-sensitivity-zero-zoom-without-change");
            }
        }

        if (!CompareExchangeFov(target.pattern, values[0], kZoomMinimumFov))
        {
            g_zoom.transitionActive = false;
            const FovRestoreResult restore = RestoreZoom();
            SetOverlayMessage(restore == FovRestoreResult::Failed ?
                L"FOV changed during validation; restore pending" :
                L"FOV changed during validation; Zoom refused", RGB(255, 170, 110));
            return;
        }
        float verify[5]{};
        if (ReadVerifiedTarget(target.options, current, verify) && fov::SameTarget(current, target) &&
            std::fabs(verify[0] - kZoomMinimumFov) < kFovMatchTolerance &&
            std::fabs(verify[2] - values[2]) < kFovMatchTolerance)
        {
            g_zoomRestorePending = false;
            InterlockedExchange(&g_pendingZoomWheel, 0);
            g_zoomWheelRemainder = 0;
            LogZoomStatus("zoom-started");
            SetOverlayMessage(sensitivityApplied ?
                L"Zooming; mouse sensitivity adjusted, release Zoom key to restore" :
                L"Zooming; mouse sensitivity unchanged, release Zoom key to restore", RGB(135, 255, 170));
            return;
        }

        const FovRestoreResult restore = RestoreZoom();
        g_zoomRestorePending = restore == FovRestoreResult::Failed;
        if (restore == FovRestoreResult::Failed)
        {
            SetOverlayMessage(L"Zoom write check failed; restore still pending", RGB(255, 170, 110));
        }
        else if (restore == FovRestoreResult::ValueChanged)
        {
            SetOverlayMessage(L"FOV changed elsewhere; value left untouched", RGB(255, 220, 120));
        }
        else
        {
            SetOverlayMessage(L"Zoom write check failed; original value restored", RGB(255, 170, 110));
        }
    }

    void ApplyZoom(HWND gameWindow)
    {
        if (!g_supportedBuild)
        {
            SetOverlayMessage(L"Unsupported Minecraft build; no memory changed", RGB(255, 170, 110));
        }
        else if (g_fovScan.result == FovScanResult::Unique)
        {
            ApplyZoomAtTarget(gameWindow, g_fovScan.foundTarget);
        }
        else
        {
            SetOverlayMessage(L"Verifying FOV automatically; Zoom waits for a unique target", RGB(255, 220, 120));
        }
    }

    void ClearZoomWheelInput()
    {
        InterlockedExchange(&g_pendingZoomWheel, 0);
        g_zoomWheelRemainder = 0;
    }

    void ApplyZoomWheel(HWND gameWindow, bool f6Down)
    {
        const LONG pending = InterlockedExchange(&g_pendingZoomWheel, 0);
        if (!g_zoom.active || g_zoom.restoring || g_zoomRestorePending || !f6Down || !IsGameActive(gameWindow))
        {
            g_zoomWheelRemainder = 0;
            return;
        }

        const LONG accumulated = g_zoomWheelRemainder + pending;
        const LONG steps = accumulated / WHEEL_DELTA;
        g_zoomWheelRemainder = accumulated - steps * WHEEL_DELTA;
        if (steps == 0) return;

        fov::Target current;
        float values[5]{};
        if (!ReadVerifiedTarget(g_zoom.target.options, current, values) ||
            !fov::SameTarget(current, g_zoom.target) ||
            std::fabs(values[0] - kZoomMinimumFov) >= kFovMatchTolerance ||
            std::fabs(values[2] - g_zoom.displayedValue) >= kFovMatchTolerance)
        {
            const FovRestoreResult restore = RestoreZoom();
            SetOverlayMessage(restore == FovRestoreResult::Failed ?
                L"Zoom restore pending; mouse wheel paused" : L"FOV changed elsewhere; value left untouched",
                restore == FovRestoreResult::Failed ? RGB(255, 120, 120) : RGB(255, 220, 120));
            ClearZoomWheelInput();
            return;
        }

        const float requested = g_zoom.currentZoomValue -
            static_cast<float>(steps) * kZoomWheelStep;
        const float nextValue = (std::clamp)(requested, kZoomMinimumFov, kZoomMaximumFov);
        if (std::fabs(nextValue - g_zoom.transitionTargetValue) < kFovMatchTolerance) return;

        const uintptr_t valueAddress = g_zoom.target.pattern + sizeof(float) * 2;
        float liveValues[5]{};
        if (!ReadFovPattern(g_zoom.target.pattern, liveValues) ||
            std::fabs(liveValues[2] - g_zoom.displayedValue) >= kFovMatchTolerance)
        {
            const FovRestoreResult restore = RestoreZoom();
            SetOverlayMessage(restore == FovRestoreResult::Failed ?
                L"Zoom restore pending; mouse wheel paused" : L"FOV changed elsewhere; value left untouched",
                restore == FovRestoreResult::Failed ? RGB(255, 120, 120) : RGB(255, 220, 120));
            ClearZoomWheelInput();
            return;
        }

        g_zoom.currentZoomValue = nextValue;
        if (g_zoom.transitionActive)
        {
            g_zoom.transitionStartValue = g_zoom.displayedValue;
            g_zoom.transitionTargetValue = nextValue;
            g_zoom.transitionStartedAt = GetTickCount64();
        }
        else
        {
            if (!CompareExchangeFov(valueAddress, g_zoom.displayedValue, nextValue))
            {
                const FovRestoreResult restore = RestoreZoom();
                SetOverlayMessage(restore == FovRestoreResult::Failed ?
                    L"Zoom restore pending; mouse wheel paused" : L"FOV changed elsewhere; value left untouched",
                    restore == FovRestoreResult::Failed ? RGB(255, 120, 120) : RGB(255, 220, 120));
                ClearZoomWheelInput();
                return;
            }
            g_zoom.displayedValue = nextValue;
            g_zoom.transitionStartValue = nextValue;
            g_zoom.transitionTargetValue = nextValue;
            g_zoom.restoring = false;
        }
        float verify[5]{};
        if (!ReadVerifiedTarget(g_zoom.target.options, current, verify) ||
            !fov::SameTarget(current, g_zoom.target) ||
            std::fabs(verify[0] - kZoomMinimumFov) >= kFovMatchTolerance ||
            std::fabs(verify[2] - g_zoom.displayedValue) >= kFovMatchTolerance)
        {
            g_zoomRestorePending = true;
            SetOverlayMessage(L"Zoom update check failed; restore pending", RGB(255, 120, 120));
            return;
        }

        LogZoomStatus("zoom-wheel");
        SetOverlayMessage(L"Zoom adjusted; scroll to tune, release Zoom key to restore", RGB(135, 255, 170));
    }

    bool ShouldCaptureZoomWheel()
    {
        return g_zoom.active && !g_zoom.restoring && !g_zoomRestorePending &&
            g_trackedGameWindow != nullptr &&
            IsZoomKeyDown() &&
            (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0 && IsGameActive(g_trackedGameWindow);
    }

    void CaptureLegacyWheel(SHORT delta)
    {
        while (InterlockedCompareExchange(&g_wheelDedupeLock, 1, 0) != 0) YieldProcessor();
        if (InterlockedCompareExchange(&g_rawWheelInputActive, 0, 0) == 0)
        {
            InterlockedExchange(&g_lastLegacyWheelDelta, delta);
            InterlockedExchange64(&g_lastLegacyWheelAt, static_cast<LONG64>(GetTickCount64()));
            InterlockedAdd(&g_pendingZoomWheel, delta);
        }
        InterlockedExchange(&g_wheelDedupeLock, 0);
    }

    void CaptureRawWheel(SHORT delta)
    {
        while (InterlockedCompareExchange(&g_wheelDedupeLock, 1, 0) != 0) YieldProcessor();
        const ULONGLONG now = GetTickCount64();
        const bool pairedLegacyEvent = InterlockedCompareExchange(&g_rawWheelInputActive, 0, 0) == 0 &&
            delta == InterlockedCompareExchange(&g_lastLegacyWheelDelta, 0, 0) &&
            now - static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_lastLegacyWheelAt, 0, 0)) <= 50;
        InterlockedExchange(&g_rawWheelInputActive, 1);
        if (!pairedLegacyEvent) InterlockedAdd(&g_pendingZoomWheel, delta);
        InterlockedExchange(&g_wheelDedupeLock, 0);
    }

    void FilterRawInputWheel(RAWINPUT& input)
    {
        if (input.header.dwType != RIM_TYPEMOUSE || !ShouldCaptureZoomWheel()) return;
        RAWMOUSE& mouse = input.data.mouse;
        if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) == 0) return;

        const SHORT delta = static_cast<SHORT>(mouse.usButtonData);
        if (delta == 0) return;
        CaptureRawWheel(delta);
        if (InterlockedCompareExchange(&g_rawWheelEventLogged, 1, 0) == 0)
        {
            LogZoomStatus("raw-wheel-filtered");
        }
        mouse.usButtonFlags = static_cast<USHORT>(mouse.usButtonFlags & ~RI_MOUSE_WHEEL);
        mouse.usButtonData = 0;
    }

    void FilterRawInputZoomKey(RAWINPUT& input)
    {
        static volatile LONG suppressedLogged = 0;
        if (input.header.dwType != RIM_TYPEKEYBOARD || !IsGameActive(g_trackedGameWindow)) return;

        RAWKEYBOARD& keyboard = input.data.keyboard;
        if (keyboard.VKey != static_cast<USHORT>(g_zoomConfig.zoomKey)) return;
        keyboard.MakeCode = 0;
        keyboard.Flags = RI_KEY_BREAK;
        keyboard.VKey = 0;
        keyboard.Message = WM_KEYUP;
        keyboard.ExtraInformation = 0;
        if (InterlockedCompareExchange(&suppressedLogged, 1, 0) == 0)
        {
            LogZoomStatus("rawinput-zoom-key-suppressed");
        }
    }

    UINT WINAPI FilteredGetRawInputData(HRAWINPUT rawInput, UINT command, LPVOID data,
        PUINT size, UINT headerSize)
    {
        if (g_originalGetRawInputData == nullptr)
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return static_cast<UINT>(-1);
        }

        const UINT result = g_originalGetRawInputData(rawInput, command, data, size, headerSize);
        if (command == RID_INPUT && data != nullptr && result != static_cast<UINT>(-1) &&
            result >= sizeof(RAWINPUTHEADER))
        {
            auto* input = static_cast<RAWINPUT*>(data);
            if (input->header.dwSize <= result)
            {
                FilterRawInputWheel(*input);
                FilterRawInputZoomKey(*input);
            }
        }
        return result;
    }

    UINT WINAPI FilteredGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
    {
        if (g_originalGetRawInputBuffer == nullptr)
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return static_cast<UINT>(-1);
        }

        const UINT bufferSize = size != nullptr ? *size : 0;
        const UINT count = g_originalGetRawInputBuffer(data, size, headerSize);
        if (data == nullptr || count == static_cast<UINT>(-1) || count == 0) return count;

        auto* cursor = reinterpret_cast<BYTE*>(data);
        SIZE_T remaining = bufferSize;
        for (UINT index = 0; index < count; ++index)
        {
            if (remaining < sizeof(RAWINPUTHEADER)) break;
            auto* input = reinterpret_cast<PRAWINPUT>(cursor);
            if (input->header.dwSize < sizeof(RAWINPUTHEADER) || input->header.dwSize > remaining) break;
            FilterRawInputWheel(*input);
            FilterRawInputZoomKey(*input);

            const SIZE_T alignment = sizeof(ULONG_PTR);
            const SIZE_T advance = (input->header.dwSize + alignment - 1) & ~(alignment - 1);
            if (advance == 0 || advance > remaining) break;
            cursor += advance;
            remaining -= advance;
        }
        return count;
    }

    bool ReplaceImportAddress(uintptr_t* address, void* replacement)
    {
        if (address == nullptr || *address == reinterpret_cast<uintptr_t>(replacement) ||
            g_inputImportPatchCount >= _countof(g_inputImportPatches)) return false;
        const uintptr_t original = *address;
        DWORD oldProtection = 0;
        if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &oldProtection)) return false;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(address), replacement);
        DWORD ignored = 0;
        VirtualProtect(address, sizeof(*address), oldProtection, &ignored);
        if (g_inputImportPatchCount < _countof(g_inputImportPatches))
        {
            g_inputImportPatches[g_inputImportPatchCount++] = { address, original };
        }
        return true;
    }

    bool PatchGameInputReading(IGameInputReading* reading, bool allowV0);

    // GameInputCreate from the system GameInputRedist entry point returns the
    // V0 interface described by the Windows SDK GameInput.h used to build this
    // client. V0 includes GetRawReport before the controller-state methods,
    // placing GetMouseState at slot 16 (IUnknown slots 0-2, then 13 methods).
    constexpr SIZE_T kGameInputV0GetMouseStateVtableIndex = 16;
    constexpr SIZE_T kGameInputV0GetKeyStateVtableIndex = 15;
    // GameInput V2 removes the V0-only methods, placing GetMouseState at slot 14.
    constexpr SIZE_T kGameInputV2GetMouseStateVtableIndex = 14;
    constexpr SIZE_T kGameInputV2GetKeyStateVtableIndex = 13;
    constexpr IID kGameInputReadingV2Iid =
        { 0x65f06483, 0xdb76, 0x40b7, { 0xb7, 0x45, 0xf5, 0x91, 0xfa, 0xe5, 0x5f, 0xc9 } };

    void NoteGameInputMouseRead()
    {
        static volatile LONG calledLogged = 0;
        if (InterlockedCompareExchange(&calledLogged, 1, 0) == 0)
        {
            LogZoomStatus("gameinput-mouse-state-called");
        }

        // Keep each device's cumulative reading intact, including during Zoom.
        // Muting is done after the game computes its delta; this also leaves
        // its saved readings valid when the hooks are detached.
    }

    void __fastcall FilteredNativeMouseWheel(float delta)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        InterlockedIncrement(&g_nativeMouseCallbacks);
        if (caller == g_gameModuleBase + 0x8CE09 && ShouldCaptureZoomWheel())
        {
            static volatile LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                LogZoomStatus("native-mouse-wheel-suppressed");
        }
        else if (g_originalNativeMouseWheel != nullptr)
        {
            g_originalNativeMouseWheel(delta);
        }
        InterlockedDecrement(&g_nativeMouseCallbacks);
    }

    void __fastcall FilteredNativeMouseEvent(void* mouse, int button, int value,
        int x, int y, int dx, int dy, bool extra)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        InterlockedIncrement(&g_nativeMouseCallbacks);
        if (caller == g_gameModuleBase + 0x8CE40 && static_cast<unsigned char>(button) == 4 &&
            ShouldCaptureZoomWheel())
        {
            static volatile LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                LogZoomStatus("native-mouse-scroll-event-suppressed");
        }
        else if (g_originalNativeMouseEvent != nullptr)
        {
            g_originalNativeMouseEvent(mouse, button, value, x, y, dx, dy, extra);
        }
        InterlockedDecrement(&g_nativeMouseCallbacks);
    }

    void InstallNativeMouseWheelHooks()
    {
        if (!g_supportedBuild || (g_nativeMouseWheelTarget != nullptr && g_nativeMouseEventTarget != nullptr))
            return;
        const MH_STATUS initialization = MH_Initialize();
        if (initialization == MH_OK) g_minHookInitializedByUs = true;
        if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED) return;

        struct Hook
        {
            uintptr_t rva;
            void* detour;
            void** original;
            void** installed;
            unsigned int index;
        };
        Hook hooks[] = {
            { 0x2D1560, reinterpret_cast<void*>(&FilteredNativeMouseWheel),
                reinterpret_cast<void**>(&g_originalNativeMouseWheel), &g_nativeMouseWheelTarget, 0 },
            { 0x361AD0, reinterpret_cast<void*>(&FilteredNativeMouseEvent),
                reinterpret_cast<void**>(&g_originalNativeMouseEvent), &g_nativeMouseEventTarget, 1 }
        };
        for (const Hook& hook : hooks)
        {
            if (*hook.installed != nullptr) continue;
            void* target = reinterpret_cast<void*>(g_gameModuleBase + hook.rva);
            if (!nametag::Matches(reinterpret_cast<uintptr_t>(target), kNativeMouseEntries[hook.index], 16))
            {
                LogZoomStatus("native-mouse-wheel-profile-changed");
                continue;
            }
            const MH_STATUS created = MH_CreateHook(target, hook.detour, hook.original);
            if (created == MH_OK && MH_EnableHook(target) == MH_OK)
            {
                *hook.installed = target;
                NativeMouseHookState& state = g_nativeMouseHookStates[hook.index];
                state.replacementVerified = nametag::Read(reinterpret_cast<uintptr_t>(target),
                    state.replacement.data(), state.replacement.size());
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(target);
                *hook.original = nullptr;
                LogZoomStatus("native-mouse-wheel-hook-failed");
            }
        }
        if (g_nativeMouseWheelTarget != nullptr && g_nativeMouseEventTarget != nullptr &&
            g_nativeMouseHookStates[0].replacementVerified && g_nativeMouseHookStates[1].replacementVerified)
            LogZoomStatus("native-mouse-wheel-hooks-ready");
    }

    bool RestoreNativeMouseWheelHooks()
    {
        if (g_nativeMouseWheelTarget == nullptr && g_nativeMouseEventTarget == nullptr) return true;
        void** targets[] = { &g_nativeMouseWheelTarget, &g_nativeMouseEventTarget };
        for (unsigned int index = 0; index < std::size(targets); ++index)
        {
            if (*targets[index] == nullptr) continue;
            NativeMouseHookState& state = g_nativeMouseHookStates[index];
            const uintptr_t address = reinterpret_cast<uintptr_t>(*targets[index]);
            if (!state.disabled)
            {
                // Refuse a foreign patch instead of overwriting it on exit.
                if (!state.replacementVerified || !nametag::Matches(address,
                    state.replacement.data(), state.replacement.size())) return false;
                if (MH_DisableHook(*targets[index]) != MH_OK) return false;
                state.disabled = true;
            }
            if (!nametag::Matches(address, kNativeMouseEntries[index], 16)) return false;
        }
        if (InterlockedCompareExchange(&g_nativeMouseCallbacks, 0, 0) != 0) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<uintptr_t>(g_module) + dos->e_lfanew);
        {
            // Check callback entry/epilogue too, where the counter can be zero.
            // No allocation, logging or CRT calls while threads are frozen.
            nametag::FrozenThreads frozen;
            if (!frozen.Freeze() || !frozen.Outside(reinterpret_cast<uintptr_t>(g_module),
                nt->OptionalHeader.SizeOfImage) ||
                InterlockedCompareExchange(&g_nativeMouseCallbacks, 0, 0) != 0) return false;
        }
        for (void** target : targets)
        {
            if (*target == nullptr) continue;
            if (MH_RemoveHook(*target) != MH_OK) return false;
            *target = nullptr;
        }
        g_originalNativeMouseWheel = nullptr;
        g_originalNativeMouseEvent = nullptr;
        LogZoomStatus("native-mouse-wheel-hooks-restored");
        return true;
    }

    uintptr_t GetGameInputDeviceIdentity(void* reading)
    {
        if (reading == nullptr) return 0;
        using GetDeviceFunction = void(STDMETHODCALLTYPE*)(void*, IUnknown**);
        auto** vtable = *reinterpret_cast<void***>(reading);
        if (vtable == nullptr || vtable[5] == nullptr) return 0;
        IUnknown* device = nullptr;
        reinterpret_cast<GetDeviceFunction>(vtable[5])(reading, &device);
        const uintptr_t identity = reinterpret_cast<uintptr_t>(device);
        if (device != nullptr) device->Release();
        return identity;
    }

    void ScaleGameInputMouseMovement(void* reading, int64_t& positionX, int64_t& positionY)
    {
        const uintptr_t device = GetGameInputDeviceIdentity(reading);
        if (device == 0) return;
        const LONG scaleBasisPoints = InterlockedCompareExchange(&g_zoomMouseScalingActive, 0, 0) != 0 ?
            InterlockedCompareExchange(&g_zoomMouseScaleBasisPoints, 0, 0) : 10'000;
        AcquireSRWLockExclusive(&g_mousePositionScaleLock);
        unsigned int index = 0;
        while (index < std::size(g_gameInputMousePositionScale) &&
            g_gameInputMousePositionScale[index].device != device) ++index;
        if (index == std::size(g_gameInputMousePositionScale))
        {
            index = 0;
            while (index < std::size(g_gameInputMousePositionScale) &&
                g_gameInputMousePositionScale[index].device != 0) ++index;
        }
        if (index == std::size(g_gameInputMousePositionScale))
        {
            ReleaseSRWLockExclusive(&g_mousePositionScaleLock);
            return;
        }
        PerDeviceMousePositionScaleState& state = g_gameInputMousePositionScale[index];
        if (state.device != device)
        {
            state = {};
            state.device = device;
        }
        auto scale = [scaleBasisPoints](MousePositionScaleState& axis, int64_t position)
        {
            if (!axis.initialized)
            {
                axis.initialized = true;
                axis.lastInput = position;
                axis.filteredPosition = static_cast<long double>(position);
            }
            else
            {
                const long double delta = static_cast<long double>(position) - axis.lastInput;
                axis.lastInput = position;
                axis.filteredPosition += delta * scaleBasisPoints / 10'000.0L;
            }
            const long double filtered = axis.filteredPosition;
            if (filtered >= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
                return (std::numeric_limits<int64_t>::max)();
            if (filtered <= static_cast<long double>((std::numeric_limits<int64_t>::min)()))
                return (std::numeric_limits<int64_t>::min)();
            return static_cast<int64_t>(std::round(filtered));
        };
        positionX = scale(state.x, positionX);
        positionY = scale(state.y, positionY);
        ReleaseSRWLockExclusive(&g_mousePositionScaleLock);
    }

    bool STDMETHODCALLTYPE FilteredGameInputGetMouseState(IGameInputReading* reading, GameInputMouseState* state)
    {
        if (g_originalGameInputGetMouseState == nullptr) return false;
        const bool result = g_originalGameInputGetMouseState(reading, state);
        if (result && state != nullptr)
        {
            ScaleGameInputMouseMovement(reading, state->positionX, state->positionY);
            NoteGameInputMouseRead();
        }
        return result;
    }

    void TraceGameMouseInput(void* reading, uintptr_t caller,
        const GameInputMouseStateV2Compat& raw, const GameInputMouseStateV2Compat& returned,
        bool capture, LONG scaling)
    {
        // Read-only diagnostic for the pinned build's actual mouse consumer:
        // GetCurrentReading(Mouse, device), GetMouseState at 0x8BDF8, then
        // wheelY (+0x30) minus that device's saved wheelY at 0x8CD5F.
        // The existing mute-boundary log covers only the first callback across
        // all devices; a zero there says nothing about the other mice.
        if (!g_supportedBuild || caller < g_gameModuleBase ||
            caller - g_gameModuleBase != 0x8BDFE || g_statusLogPath.empty()) return;

        using GetDeviceFunction = void(STDMETHODCALLTYPE*)(void*, IUnknown**);
        auto** vtable = *reinterpret_cast<void***>(reading);
        IUnknown* device = nullptr;
        reinterpret_cast<GetDeviceFunction>(vtable[5])(reading, &device);
        if (device == nullptr) return;
        const uintptr_t identity = reinterpret_cast<uintptr_t>(device);
        device->Release();

        const LONG epoch = InterlockedCompareExchange(&g_zoomInputEpoch, 0, 0);
        char line[768]{};
        AcquireSRWLockExclusive(&g_mouseInputTraceLock);
        unsigned int index = 0;
        while (index < g_mouseInputTraceCount && g_mouseInputTraces[index].device != identity) ++index;
        const bool first = index == g_mouseInputTraceCount;
        if (first && g_mouseInputTraceCount == std::size(g_mouseInputTraces))
        {
            ReleaseSRWLockExclusive(&g_mouseInputTraceLock);
            return;
        }
        MouseInputTrace& previous = g_mouseInputTraces[index];
        const bool changed = first || previous.epoch != epoch || previous.capture != capture ||
            previous.scaling != scaling || previous.rawWheel != raw.wheelY ||
            previous.returnedWheel != returned.wheelY;
        int length = 0;
        if (changed && g_mouseInputTraceLines < 512)
        {
            ++g_mouseInputTraceLines;
            length = sprintf_s(line,
                "pid=%lu event=game-mouse-input elapsed_ms=%llu thread=%lu caller_rva=0x8BDFE "
                "device=0x%llX epoch=%ld first=%u capture=%u scaling=%ld "
                "raw_wheel=%lld returned_wheel=%lld previous_raw_wheel=%lld previous_returned_wheel=%lld "
                "buttons=%u positions=%u raw_x=%lld raw_y=%lld returned_x=%lld returned_y=%lld\r\n",
                GetCurrentProcessId(), GetTickCount64() - g_fovScan.startedAt, GetCurrentThreadId(),
                static_cast<unsigned long long>(identity), epoch, first ? 1u : 0u, capture ? 1u : 0u, scaling,
                static_cast<long long>(raw.wheelY), static_cast<long long>(returned.wheelY),
                static_cast<long long>(previous.rawWheel), static_cast<long long>(previous.returnedWheel),
                raw.buttons, raw.positions, static_cast<long long>(raw.positionX),
                static_cast<long long>(raw.positionY), static_cast<long long>(returned.positionX),
                static_cast<long long>(returned.positionY));
        }
        previous = { identity, epoch, capture, scaling, raw.wheelY, returned.wheelY };
        if (first) ++g_mouseInputTraceCount;
        if (length > 0)
        {
            const HANDLE file = CreateFileW(g_statusLogPath.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
                CloseHandle(file);
            }
        }
        ReleaseSRWLockExclusive(&g_mouseInputTraceLock);
    }

    bool STDMETHODCALLTYPE FilteredGameInputV2GetMouseState(void* reading, GameInputMouseStateV2Compat* state)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_originalGameInputV2GetMouseState == nullptr) return false;
        const bool result = g_originalGameInputV2GetMouseState(reading, state);
        if (result && state != nullptr)
        {
            const GameInputMouseStateV2Compat raw = *state;
            const bool capture = ShouldCaptureZoomWheel();
            const LONG scaling = InterlockedCompareExchange(&g_zoomMouseScalingActive, 0, 0);
            // GameInput V2 positionX/Y must be relative-position readings;
            // absolute screen coordinates must remain untouched.
            if ((state->positions & 0x2u) != 0)
                ScaleGameInputMouseMovement(reading, state->positionX, state->positionY);
            NoteGameInputMouseRead();
            TraceGameMouseInput(reading, caller, raw, *state, capture, scaling);
        }
        return result;
    }

    uint32_t FilterZoomKey(GameInputKeyState* stateArray, uint32_t stateArrayCount, uint32_t validCount)
    {
        static volatile LONG suppressedLogged = 0;
        if (stateArray == nullptr || !IsGameActive(g_trackedGameWindow)) return validCount;

        const uint32_t boundedCount = (std::min)(stateArrayCount, validCount);
        for (uint32_t index = 0; index < boundedCount; ++index)
        {
            if (stateArray[index].virtualKey != static_cast<uint8_t>(g_zoomConfig.zoomKey)) continue;
            if (index + 1 < boundedCount)
            {
                std::memmove(&stateArray[index], &stateArray[index + 1],
                    (boundedCount - index - 1) * sizeof(GameInputKeyState));
            }
            if (InterlockedCompareExchange(&suppressedLogged, 1, 0) == 0)
            {
                LogZoomStatus("gameinput-zoom-key-suppressed");
            }
            return validCount - 1;
        }
        return validCount;
    }

    uint32_t STDMETHODCALLTYPE FilteredGameInputGetKeyState(IGameInputReading* reading,
        uint32_t stateArrayCount, GameInputKeyState* stateArray)
    {
        if (g_originalGameInputGetKeyState == nullptr) return 0;
        const uint32_t validCount = g_originalGameInputGetKeyState(reading, stateArrayCount, stateArray);
        return FilterZoomKey(stateArray, stateArrayCount, validCount);
    }

    uint32_t STDMETHODCALLTYPE FilteredGameInputV2GetKeyState(void* reading,
        uint32_t stateArrayCount, GameInputKeyState* stateArray)
    {
        if (g_originalGameInputV2GetKeyState == nullptr) return 0;
        static volatile LONG calledLogged = 0;
        if (InterlockedCompareExchange(&calledLogged, 1, 0) == 0)
            LogZoomStatus("gameinput-v2-key-state-called");
        const uint32_t validCount = g_originalGameInputV2GetKeyState(reading, stateArrayCount, stateArray);
        return FilterZoomKey(stateArray, stateArrayCount, validCount);
    }

    bool ShouldHideZoomKeyFromGame()
    {
        return g_zoomConfig.zoomKey > 0 && g_zoomConfig.zoomKey <= 0xFF &&
            IsGameActive(g_trackedGameWindow);
    }

    SHORT WINAPI FilteredGetAsyncKeyState(int virtualKey)
    {
        if (g_originalGetAsyncKeyState == nullptr) return 0;
        if (virtualKey == g_zoomConfig.zoomKey && ShouldHideZoomKeyFromGame())
        {
            static volatile LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                LogZoomStatus("getasynckeystate-zoom-key-suppressed");
            return 0;
        }
        return g_originalGetAsyncKeyState(virtualKey);
    }

    SHORT WINAPI FilteredGetKeyState(int virtualKey)
    {
        if (g_originalGetKeyState == nullptr) return 0;
        if (virtualKey == g_zoomConfig.zoomKey && ShouldHideZoomKeyFromGame())
        {
            static volatile LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                LogZoomStatus("getkeystate-zoom-key-suppressed");
            return 0;
        }
        return g_originalGetKeyState(virtualKey);
    }

    BOOL WINAPI FilteredGetKeyboardState(PBYTE state)
    {
        if (g_originalGetKeyboardState == nullptr) return FALSE;
        const BOOL result = g_originalGetKeyboardState(state);
        if (result && state != nullptr && ShouldHideZoomKeyFromGame())
        {
            if ((state[g_zoomConfig.zoomKey] & 0x80) != 0)
            {
                static volatile LONG logged = 0;
                state[g_zoomConfig.zoomKey] = 0;
                if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                    LogZoomStatus("getkeyboardstate-zoom-key-suppressed");
            }
        }
        return result;
    }

    void InstallKeyboardStateApiHooks()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) return;
        const MH_STATUS initialization = MH_Initialize();
        if (initialization == MH_OK) g_minHookInitializedByUs = true;
        if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED) return;

        struct ApiHook
        {
            const char* name;
            void* detour;
            void** original;
            void** target;
            const char* installedEvent;
            const char* failedEvent;
        };
        ApiHook hooks[] = {
            { "GetAsyncKeyState", reinterpret_cast<void*>(&FilteredGetAsyncKeyState),
                reinterpret_cast<void**>(&g_originalGetAsyncKeyState), &g_getAsyncKeyStateTarget,
                "getasynckeystate-hooked", "getasynckeystate-hook-failed" },
            { "GetKeyState", reinterpret_cast<void*>(&FilteredGetKeyState),
                reinterpret_cast<void**>(&g_originalGetKeyState), &g_getKeyStateTarget,
                "getkeystate-hooked", "getkeystate-hook-failed" },
            { "GetKeyboardState", reinterpret_cast<void*>(&FilteredGetKeyboardState),
                reinterpret_cast<void**>(&g_originalGetKeyboardState), &g_getKeyboardStateTarget,
                "getkeyboardstate-hooked", "getkeyboardstate-hook-failed" }
        };
        for (ApiHook& hook : hooks)
        {
            if (*hook.target != nullptr) continue;
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, hook.name));
            if (target == nullptr) continue;
            const MH_STATUS created = MH_CreateHook(target, hook.detour, hook.original);
            if (created == MH_OK && MH_EnableHook(target) == MH_OK)
            {
                *hook.target = target;
                LogZoomStatus(hook.installedEvent);
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(target);
                LogZoomStatus(hook.failedEvent);
            }
        }
    }

    bool PatchGameInputReading(IGameInputReading* reading, bool allowV0)
    {
        if (reading == nullptr) return false;

        void* v2Reading = nullptr;
        if (SUCCEEDED(reading->QueryInterface(kGameInputReadingV2Iid, &v2Reading)) && v2Reading != nullptr)
        {
            void** v2Vtable = *reinterpret_cast<void***>(v2Reading);
            void* v2Target = v2Vtable[kGameInputV2GetMouseStateVtableIndex];
            if (v2Target != nullptr && g_gameInputV2MouseStateTarget == nullptr)
            {
                const MH_STATUS initialization = MH_Initialize();
                if (initialization == MH_OK) g_minHookInitializedByUs = true;
                if (initialization == MH_OK || initialization == MH_ERROR_ALREADY_INITIALIZED)
                {
                    const MH_STATUS created = MH_CreateHook(v2Target,
                        reinterpret_cast<void*>(&FilteredGameInputV2GetMouseState),
                        reinterpret_cast<void**>(&g_originalGameInputV2GetMouseState));
                    if (created == MH_OK && MH_EnableHook(v2Target) == MH_OK)
                    {
                        g_gameInputV2MouseStateTarget = v2Target;
                        LogZoomStatus("gameinput-v2-interface-hooked");
                    }
                    else
                    {
                        if (created == MH_OK) MH_RemoveHook(v2Target);
                        g_originalGameInputV2GetMouseState = nullptr;
                        LogZoomStatus("gameinput-v2-inline-hook-failed");
                    }
                }
                else
                {
                    LogZoomStatus("gameinput-v2-inline-hook-init-failed");
                }
            }
            void* v2KeyTarget = v2Vtable[kGameInputV2GetKeyStateVtableIndex];
            if (v2KeyTarget != nullptr && g_gameInputV2KeyStateTarget == nullptr)
            {
                const MH_STATUS initialization = MH_Initialize();
                if (initialization == MH_OK) g_minHookInitializedByUs = true;
                if (initialization == MH_OK || initialization == MH_ERROR_ALREADY_INITIALIZED)
                {
                    const MH_STATUS created = MH_CreateHook(v2KeyTarget,
                        reinterpret_cast<void*>(&FilteredGameInputV2GetKeyState),
                        reinterpret_cast<void**>(&g_originalGameInputV2GetKeyState));
                    if (created == MH_OK && MH_EnableHook(v2KeyTarget) == MH_OK)
                    {
                        g_gameInputV2KeyStateTarget = v2KeyTarget;
                        LogZoomStatus("gameinput-v2-keyboard-hooked");
                    }
                    else
                    {
                        if (created == MH_OK) MH_RemoveHook(v2KeyTarget);
                        g_originalGameInputV2GetKeyState = nullptr;
                        LogZoomStatus("gameinput-v2-keyboard-hook-failed");
                    }
                }
                else
                {
                    LogZoomStatus("gameinput-v2-keyboard-hook-init-failed");
                }
            }
            static_cast<IUnknown*>(v2Reading)->Release();
            if (!allowV0 ||
                (g_gameInputV2MouseStateTarget != nullptr && g_gameInputV2KeyStateTarget != nullptr))
            {
                return g_gameInputV2MouseStateTarget != nullptr || g_gameInputV2KeyStateTarget != nullptr;
            }
        }
        else
        {
            static volatile LONG v2UnavailableLogged = 0;
            if (InterlockedCompareExchange(&v2UnavailableLogged, 1, 0) == 0)
            {
                LogZoomStatus("gameinput-v2-interface-unavailable");
            }
        }
        if (!allowV0) return false;

        void** vtable = *reinterpret_cast<void***>(reading);
        const MH_STATUS initialization = MH_Initialize();
        if (initialization == MH_OK) g_minHookInitializedByUs = true;
        if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED)
        {
            LogZoomStatus("gameinput-inline-hook-init-failed");
            return false;
        }

        void* mouseTarget = vtable[kGameInputV0GetMouseStateVtableIndex];
        if (mouseTarget != nullptr && g_gameInputMouseStateTarget == nullptr)
        {
            const MH_STATUS created = MH_CreateHook(mouseTarget,
                reinterpret_cast<void*>(&FilteredGameInputGetMouseState),
                reinterpret_cast<void**>(&g_originalGameInputGetMouseState));
            if (created == MH_OK && MH_EnableHook(mouseTarget) == MH_OK)
            {
                g_gameInputMouseStateTarget = mouseTarget;
                LogZoomStatus("gameinput-inline-hooked");
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(mouseTarget);
                g_originalGameInputGetMouseState = nullptr;
                LogZoomStatus("gameinput-inline-hook-failed");
            }
        }

        void* keyTarget = vtable[kGameInputV0GetKeyStateVtableIndex];
        if (keyTarget != nullptr && g_gameInputKeyStateTarget == nullptr)
        {
            const MH_STATUS created = MH_CreateHook(keyTarget,
                reinterpret_cast<void*>(&FilteredGameInputGetKeyState),
                reinterpret_cast<void**>(&g_originalGameInputGetKeyState));
            if (created == MH_OK && MH_EnableHook(keyTarget) == MH_OK)
            {
                g_gameInputKeyStateTarget = keyTarget;
                LogZoomStatus("gameinput-keyboard-hooked");
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(keyTarget);
                g_originalGameInputGetKeyState = nullptr;
                LogZoomStatus("gameinput-keyboard-hook-failed");
            }
        }
        return g_gameInputMouseStateTarget != nullptr || g_gameInputKeyStateTarget != nullptr;
    }

    bool InstallGameInputApiHooks()
    {
        InstallNativeMouseWheelHooks();
        InstallKeyboardStateApiHooks();
        if ((g_gameInputMouseStateTarget != nullptr || g_gameInputV2MouseStateTarget != nullptr) &&
            (g_gameInputKeyStateTarget != nullptr || g_gameInputV2KeyStateTarget != nullptr)) return false;

        HMODULE modules[] = { GetModuleHandleW(L"GameInput.dll"), GetModuleHandleW(L"GameInputRedist.dll") };
        auto tryModules = [&](bool allowV0)
        {
            for (SIZE_T index = 0; index < _countof(modules); ++index)
            {
                HMODULE module = modules[index];
                if (module == nullptr || (index > 0 && module == modules[0])) continue;
                const auto create = reinterpret_cast<GameInputCreateFunction>(
                    GetProcAddress(module, "GameInputCreate"));
                if (create == nullptr) continue;

                IGameInput* gameInput = nullptr;
                const HRESULT result = create(&gameInput);
                if (FAILED(result) || gameInput == nullptr) continue;
                IGameInputReading* mouseReading = nullptr;
                const HRESULT readingResult = gameInput->GetCurrentReading(GameInputKindMouse, nullptr, &mouseReading);
                if (SUCCEEDED(readingResult) && mouseReading != nullptr)
                {
                    PatchGameInputReading(mouseReading, allowV0);
                    mouseReading->Release();
                }
                IGameInputReading* keyboardReading = nullptr;
                const HRESULT keyboardResult = gameInput->GetCurrentReading(
                    GameInputKindKeyboard, nullptr, &keyboardReading);
                if (SUCCEEDED(keyboardResult) && keyboardReading != nullptr)
                {
                    PatchGameInputReading(keyboardReading, allowV0);
                    keyboardReading->Release();
                }
                gameInput->Release();
                const bool mouseHooked = g_gameInputMouseStateTarget != nullptr ||
                    g_gameInputV2MouseStateTarget != nullptr;
                const bool keyboardHooked = g_gameInputKeyStateTarget != nullptr ||
                    g_gameInputV2KeyStateTarget != nullptr;
                if (mouseHooked && keyboardHooked) return true;
            }
            return false;
        };

        // Bedrock GDK uses GameInput V2. Try V2 across all loaded runtimes
        // before accepting a legacy V0 reading from a compatibility DLL.
        if (tryModules(false)) return true;
        return tryModules(true);
    }

    void RestoreGameInputHooks()
    {
        for (unsigned int index = g_inputImportPatchCount; index > 0; --index)
        {
            ImportAddressPatch& patch = g_inputImportPatches[index - 1];
            if (patch.slot != nullptr)
            {
                DWORD oldProtection = 0;
                if (VirtualProtect(patch.slot, sizeof(uintptr_t), PAGE_READWRITE, &oldProtection))
                {
                    const uintptr_t current = *patch.slot;
                    if (current == reinterpret_cast<uintptr_t>(&FilteredGetRawInputData) ||
                        current == reinterpret_cast<uintptr_t>(&FilteredGetRawInputBuffer))
                    {
                        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(patch.slot),
                            reinterpret_cast<void*>(patch.original));
                    }
                    DWORD ignored = 0;
                    VirtualProtect(patch.slot, sizeof(uintptr_t), oldProtection, &ignored);
                }
            }
            patch = {};
        }
        g_inputImportPatchCount = 0;

        for (void* target : { g_gameInputMouseStateTarget, g_gameInputV2MouseStateTarget,
            g_gameInputKeyStateTarget, g_gameInputV2KeyStateTarget,
            g_rawInputDataTarget, g_rawInputBufferTarget, g_getAsyncKeyStateTarget,
            g_getKeyStateTarget, g_getKeyboardStateTarget })
        {
            if (target != nullptr)
            {
                MH_DisableHook(target);
                MH_RemoveHook(target);
            }
        }
        g_gameInputMouseStateTarget = nullptr;
        g_gameInputV2MouseStateTarget = nullptr;
        g_gameInputKeyStateTarget = nullptr;
        g_gameInputV2KeyStateTarget = nullptr;
        g_rawInputDataTarget = nullptr;
        g_rawInputBufferTarget = nullptr;
        g_getAsyncKeyStateTarget = nullptr;
        g_getKeyStateTarget = nullptr;
        g_getKeyboardStateTarget = nullptr;
        g_originalGameInputGetMouseState = nullptr;
        g_originalGameInputV2GetMouseState = nullptr;
        g_originalGameInputGetKeyState = nullptr;
        g_originalGameInputV2GetKeyState = nullptr;
        g_originalGetRawInputData = nullptr;
        g_originalGetRawInputBuffer = nullptr;
        g_originalGetAsyncKeyState = nullptr;
        g_originalGetKeyState = nullptr;
        g_originalGetKeyboardState = nullptr;
        if (g_minHookInitializedByUs)
        {
            MH_Uninitialize();
            g_minHookInitializedByUs = false;
        }
    }

    unsigned int PatchRawInputImports(HMODULE module)
    {
        if (module == nullptr) return 0;
        unsigned int patched = 0;
        __try
        {
            auto* base = reinterpret_cast<BYTE*>(module);
            auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
            auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                return 0;

            const IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (imports.VirtualAddress == 0 || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;
            auto* descriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + imports.VirtualAddress);
            for (; descriptor->Name != 0 && descriptor->FirstThunk != 0; ++descriptor)
            {
                auto* names = descriptor->OriginalFirstThunk != 0 ?
                    reinterpret_cast<PIMAGE_THUNK_DATA64>(base + descriptor->OriginalFirstThunk) : nullptr;
                auto* addresses = reinterpret_cast<PIMAGE_THUNK_DATA64>(base + descriptor->FirstThunk);
                for (SIZE_T index = 0; addresses[index].u1.Function != 0; ++index)
                {
                    if (names == nullptr)
                    {
                        const uintptr_t importedAddress = static_cast<uintptr_t>(addresses[index].u1.Function);
                        if (importedAddress == reinterpret_cast<uintptr_t>(g_originalGetRawInputData) &&
                            ReplaceImportAddress(reinterpret_cast<uintptr_t*>(&addresses[index].u1.Function),
                                reinterpret_cast<void*>(&FilteredGetRawInputData)))
                        {
                            ++patched;
                        }
                        else if (importedAddress == reinterpret_cast<uintptr_t>(g_originalGetRawInputBuffer) &&
                            ReplaceImportAddress(reinterpret_cast<uintptr_t*>(&addresses[index].u1.Function),
                                reinterpret_cast<void*>(&FilteredGetRawInputBuffer)))
                        {
                            ++patched;
                        }
                        continue;
                    }
                    if (IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) continue;
                    auto* importName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + names[index].u1.AddressOfData);
                    const char* functionName = reinterpret_cast<const char*>(importName->Name);
                    if (std::strcmp(functionName, "GetRawInputData") == 0 &&
                        ReplaceImportAddress(reinterpret_cast<uintptr_t*>(&addresses[index].u1.Function),
                            reinterpret_cast<void*>(&FilteredGetRawInputData)))
                    {
                        ++patched;
                    }
                    else if (std::strcmp(functionName, "GetRawInputBuffer") == 0 &&
                        ReplaceImportAddress(reinterpret_cast<uintptr_t*>(&addresses[index].u1.Function),
                            reinterpret_cast<void*>(&FilteredGetRawInputBuffer)))
                    {
                        ++patched;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return patched;
        }
        return patched;
    }

    unsigned int InstallRawInputApiHooks()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) return 0;
        void* rawInputData = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));
        void* rawInputBuffer = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputBuffer"));
        if (rawInputData == nullptr || rawInputBuffer == nullptr) return 0;

        const MH_STATUS initialization = MH_Initialize();
        if (initialization == MH_OK) g_minHookInitializedByUs = true;
        else if (initialization != MH_ERROR_ALREADY_INITIALIZED)
        {
            LogZoomStatus("raw-input-inline-hook-init-failed");
            return 0;
        }

        bool inlineHookAdded = false;
        if (g_rawInputDataTarget == nullptr)
        {
            const MH_STATUS created = MH_CreateHook(rawInputData,
                reinterpret_cast<void*>(&FilteredGetRawInputData),
                reinterpret_cast<void**>(&g_originalGetRawInputData));
            if (created == MH_OK && MH_EnableHook(rawInputData) == MH_OK)
            {
                g_rawInputDataTarget = rawInputData;
                inlineHookAdded = true;
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(rawInputData);
                g_originalGetRawInputData = nullptr;
                LogZoomStatus("raw-input-data-inline-hook-failed");
            }
        }
        if (g_rawInputBufferTarget == nullptr)
        {
            const MH_STATUS created = MH_CreateHook(rawInputBuffer,
                reinterpret_cast<void*>(&FilteredGetRawInputBuffer),
                reinterpret_cast<void**>(&g_originalGetRawInputBuffer));
            if (created == MH_OK && MH_EnableHook(rawInputBuffer) == MH_OK)
            {
                g_rawInputBufferTarget = rawInputBuffer;
                inlineHookAdded = true;
            }
            else
            {
                if (created == MH_OK) MH_RemoveHook(rawInputBuffer);
                g_originalGetRawInputBuffer = nullptr;
                LogZoomStatus("raw-input-buffer-inline-hook-failed");
            }
        }
        if (inlineHookAdded) LogZoomStatus("raw-input-inline-hooked");

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) return 0;

        unsigned int patched = 0;
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                patched += PatchRawInputImports(entry.hModule);
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return patched;
    }

    LRESULT CALLBACK GameInputMessageHookProc(int code, WPARAM removeFlag, LPARAM data);

    unsigned int InstallGameInputMessageHooks()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;

        unsigned int installed = 0;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                    entry.th32ThreadID == GetCurrentThreadId()) continue;
                const bool alreadyHooked = std::any_of(g_gameInputHooks.begin(), g_gameInputHooks.end(),
                    [&entry](const GameInputHook& hook) { return hook.threadId == entry.th32ThreadID; });
                if (alreadyHooked) continue;

                HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, GameInputMessageHookProc,
                    g_module, entry.th32ThreadID);
                if (hook != nullptr)
                {
                    g_gameInputHooks.push_back({entry.th32ThreadID, hook});
                    ++installed;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return installed;
    }

    bool FilterZoomWheelMessage(MSG& message, bool capture)
    {
        if (!capture || (message.message != WM_MOUSEWHEEL && message.message != WM_POINTERWHEEL))
            return false;

        // Both message types carry a signed wheel delta in the high word.
        // Capture only input already delivered to this game's message queue;
        // a global low-level mouse hook would make desktop motion wait for
        // our worker's memory scans, hook installation and sleep intervals.
        const SHORT delta = static_cast<SHORT>(HIWORD(message.wParam));
        if (delta == 0) return false;
        CaptureLegacyWheel(delta);
        const bool pointer = message.message == WM_POINTERWHEEL;
        message.message = WM_NULL;
        message.wParam = 0;
        message.lParam = 0;
        volatile LONG* logged = pointer ? &g_pointerWheelEventLogged : &g_messageWheelEventLogged;
        if (InterlockedCompareExchange(logged, 1, 0) == 0)
            LogZoomStatus(pointer ? "pointer-wheel-suppressed" : "message-wheel-suppressed");
        return true;
    }

    LRESULT CALLBACK GameInputMessageHookProc(int code, WPARAM removeFlag, LPARAM data)
    {
        if (code >= 0 && removeFlag == PM_REMOVE && data != 0)
        {
            auto* message = reinterpret_cast<MSG*>(data);
            if (ShouldHideZoomKeyFromGame() &&
                (message->message == WM_KEYDOWN || message->message == WM_KEYUP ||
                    message->message == WM_SYSKEYDOWN || message->message == WM_SYSKEYUP) &&
                message->wParam == static_cast<WPARAM>(g_zoomConfig.zoomKey))
            {
                static volatile LONG suppressedLogged = 0;
                message->message = WM_NULL;
                message->wParam = 0;
                message->lParam = 0;
                if (InterlockedCompareExchange(&suppressedLogged, 1, 0) == 0)
                    LogZoomStatus("window-message-zoom-key-suppressed");
                return 0;
            }
        }
        if (code >= 0 && removeFlag == PM_REMOVE && data != 0)
        {
            auto* message = reinterpret_cast<MSG*>(data);
            if ((message->message == WM_MOUSEWHEEL || message->message == WM_POINTERWHEEL) &&
                FilterZoomWheelMessage(*message, ShouldCaptureZoomWheel()))
                return 0;
        }

        return CallNextHookEx(nullptr, code, removeFlag, data);
    }

    LRESULT CALLBACK ZoomKeyboardHookProc(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && data != 0)
        {
            const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (keyboard->vkCode == static_cast<DWORD>(g_zoomConfig.zoomKey))
            {
                const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
                const bool gameActive = IsGameActive(g_trackedGameWindow);
                bool stateChanged = false;
                if (keyUp) stateChanged = InterlockedExchange(&g_zoomKeyDown, 0) != 0;
                if (keyDown && gameActive) stateChanged = InterlockedExchange(&g_zoomKeyDown, 1) == 0;
                if (stateChanged && gameActive)
                {
                    InterlockedIncrement(&g_zoomInputEpoch);
                    LogZoomStatus(keyDown ? "zoom-key-down" : "zoom-key-up");
                }
                if ((keyDown || keyUp) && gameActive)
                {
                    // The configured Zoom shortcut is handled locally; letting it
                    // reach the game can also trigger a held gameplay binding.
                    return 1;
                }
            }
        }
        return CallNextHookEx(g_zoomKeyboardHook, code, message, data);
    }

    void BeginZoomRestoreTransition()
    {
        if (!g_zoom.active || g_zoom.restoring || g_zoomRestorePending) return;
        g_zoom.transitionStartValue = g_zoom.displayedValue;
        g_zoom.transitionTargetValue = g_zoom.originalValue;
        g_zoom.transitionStartedAt = GetTickCount64();
        g_zoom.transitionActive = true;
        g_zoom.restoring = true;
    }

    void AdvanceZoomTransition()
    {
        if (!g_zoom.active || !g_zoom.transitionActive || g_zoomRestorePending) return;

        const ULONGLONG now = GetTickCount64();
        const float progress = (std::min)(1.0f,
            static_cast<float>(now - g_zoom.transitionStartedAt) /
            static_cast<float>(g_zoomConfig.transitionDurationMs));
        // Smoothstep gives the transition a soft start and stop.
        const float eased = progress * progress * (3.0f - 2.0f * progress);
        const float nextValue = g_zoom.transitionStartValue +
            (g_zoom.transitionTargetValue - g_zoom.transitionStartValue) * eased;
        if (std::fabs(nextValue - g_zoom.displayedValue) >= kFovMatchTolerance)
        {
            const uintptr_t valueAddress = g_zoom.target.pattern + sizeof(float) * 2;
            if (!CompareExchangeFov(valueAddress, g_zoom.displayedValue, nextValue))
            {
                g_zoomRestorePending = true;
                return;
            }
            g_zoom.displayedValue = nextValue;
        }

        if (progress >= 1.0f)
        {
            g_zoom.displayedValue = g_zoom.transitionTargetValue;
            if (!g_zoom.restoring) g_zoom.currentZoomValue = g_zoom.transitionTargetValue;
            g_zoom.transitionActive = false;
            if (g_zoom.restoring)
            {
                const FovRestoreResult restore = RestoreZoom();
                SetOverlayMessage(restore == FovRestoreResult::Restored ?
                    L"Original FOV restored" : restore == FovRestoreResult::ValueChanged ?
                    L"FOV changed elsewhere; value left untouched" :
                    L"Zoom restore pending; module remains active",
                    restore == FovRestoreResult::Failed ? RGB(255, 120, 120) : RGB(135, 255, 170));
            }
            else
            {
                LogZoomStatus("zoom-started");
                SetOverlayMessage(L"Zoom active; scroll to adjust, release Zoom key to restore", RGB(135, 255, 170));
            }
        }
    }

    void FinishFovScan(FovScanResult result)
    {
        g_fovScan.result = result;
        g_fovScan.finishedAt = GetTickCount64();
        InterlockedExchange(&g_zoomReadiness, result == FovScanResult::Unique ? 2 : 3);
        const char* event = "refused";
        switch (result)
        {
        case FovScanResult::Unique:
            event = "ready";
            SetOverlayMessage(L"Zoom ready; shortcuts are configurable in bedrock-toolkit.ini", RGB(135, 255, 170));
            break;
        case FovScanResult::None:
            event = "not-found";
            SetOverlayMessage(L"FOV not initialized yet; automatic retry, no memory changed", RGB(255, 220, 120));
            break;
        case FovScanResult::Multiple:
            event = "ambiguous";
            SetOverlayMessage(L"FOV search was ambiguous; no memory changed", RGB(255, 120, 120));
            break;
        case FovScanResult::TimedOut:
            event = "timeout";
            SetOverlayMessage(L"FOV search timed out; no memory changed", RGB(255, 170, 110));
            break;
        case FovScanResult::ReadFailure:
            event = "read-failed";
            SetOverlayMessage(L"FOV search incomplete; automatic retry, no memory changed", RGB(255, 170, 110));
            break;
        case FovScanResult::QueryFailure:
            event = "query-failed";
            SetOverlayMessage(L"FOV memory map changed; automatic retry, no memory changed", RGB(255, 170, 110));
            break;
        default:
            break;
        }
        LogZoomStatus(event);
    }

    bool StartFovScan()
    {
        if (!g_supportedBuild) return false;
        if (g_fovScan.result == FovScanResult::Running || g_fovScan.result == FovScanResult::Verifying) return true;
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        if (maxAddress == (std::numeric_limits<uintptr_t>::max)())
        {
            FinishFovScan(FovScanResult::QueryFailure);
            return false;
        }
        g_fovScan = {};
        try
        {
            g_fovScan.buffer.resize(kFovScanChunkBytes);
        }
        catch (...)
        {
            FinishFovScan(FovScanResult::ReadFailure);
            return false;
        }
        g_fovScan.cursor = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        g_fovScan.maxAddressExclusive = maxAddress + 1;
        g_fovScan.startedAt = GetTickCount64();
        g_fovScan.result = FovScanResult::Running;
        InterlockedExchange(&g_zoomReadiness, 1);
        SetOverlayMessage(L"Preparing Zoom automatically", RGB(255, 220, 120));
        LogZoomStatus("search-start");
        return true;
    }

    void AdvanceFovScan()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_fovScan.result == FovScanResult::Verifying)
        {
            if (now - g_fovScan.finishedAt < kFovVerifyDelayMs) return;
            fov::Target current;
            float values[5]{};
            if (ReadVerifiedTarget(g_fovScan.foundTarget.options, current, values) &&
                fov::SameTarget(current, g_fovScan.foundTarget))
            {
                FinishFovScan(FovScanResult::Unique);
            }
            else
            {
                FinishFovScan(FovScanResult::None);
            }
            return;
        }
        if (g_fovScan.result != FovScanResult::Running) return;
        if (now - g_fovScan.startedAt > kFovScanTimeoutMs)
        {
            FinishFovScan(FovScanResult::TimedOut);
            return;
        }

        // Bounded work keeps Zoom-key release, Escape, focus loss and exit responsive.
        SIZE_T byteBudget = 32 * 1024 * 1024;
        unsigned int regionBudget = 1024;
        while (g_fovScan.cursor < g_fovScan.maxAddressExclusive && byteBudget > 0 && regionBudget-- > 0)
        {
            if (GetTickCount64() - now >= 4) break;
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<const void*>(g_fovScan.cursor), &memory, sizeof(memory)) == 0)
            {
                FinishFovScan(FovScanResult::QueryFailure);
                return;
            }
            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
            if (memory.RegionSize == 0 || regionBase >= g_fovScan.maxAddressExclusive)
            {
                FinishFovScan(FovScanResult::QueryFailure);
                return;
            }
            const uintptr_t regionEnd = regionBase + std::min<uintptr_t>(memory.RegionSize, g_fovScan.maxAddressExclusive - regionBase);
            if (regionEnd <= g_fovScan.cursor)
            {
                FinishFovScan(FovScanResult::QueryFailure);
                return;
            }
            if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE || memory.Protect != PAGE_READWRITE)
            {
                g_fovScan.cursor = regionEnd;
                continue;
            }
            const SIZE_T bytes = static_cast<SIZE_T>(std::min<uintptr_t>(kFovScanChunkBytes, regionEnd - g_fovScan.cursor));
            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(g_fovScan.cursor),
                g_fovScan.buffer.data(), bytes, &bytesRead) || bytesRead != bytes)
            {
                FinishFovScan(FovScanResult::ReadFailure);
                return;
            }
            // A numeric FOV-like copy is insufficient: look for the actual
            // Options type, its FOV field, FloatOption type and all three keys.
            const bool completed = fov::FindOptions(g_fovScan.buffer.data(), bytes, g_fovScan.cursor,
                g_gameModuleBase + fov::kOptionsVtableRva, [](uintptr_t options)
                {
                    const uintptr_t scratch = reinterpret_cast<uintptr_t>(g_fovScan.buffer.data());
                    if (options >= scratch && options - scratch < g_fovScan.buffer.size()) return true;
                    fov::Target target;
                    float values[5]{};
                    if (!ReadVerifiedTarget(options, target, values)) return true;
                    ++g_fovScan.matches;
                    if (g_fovScan.matches > 1) return false;
                    g_fovScan.foundTarget = target;
                    return true;
                });
            if (!completed)
            {
                FinishFovScan(FovScanResult::Multiple);
                return;
            }
            g_fovScan.bytesScanned += bytes;
            byteBudget -= bytes;
            g_fovScan.cursor += bytes;
        }
        if (g_fovScan.cursor >= g_fovScan.maxAddressExclusive)
        {
            if (g_fovScan.matches == 1)
            {
                g_fovScan.result = FovScanResult::Verifying;
                g_fovScan.finishedAt = GetTickCount64();
                LogZoomStatus("scan-complete");
            }
            else
            {
                FinishFovScan(FovScanResult::None);
            }
        }
    }

    void UpdateFovDiscovery()
    {
        if (!g_supportedBuild || g_zoomRestorePending) return;
        if (g_fovScan.result == FovScanResult::Unique)
        {
            fov::Target current;
            float values[5]{};
            if (ReadVerifiedTarget(g_fovScan.foundTarget.options, current, values) &&
                fov::SameTarget(current, g_fovScan.foundTarget)) return;
            if (g_zoom.active)
            {
                RestoreZoom();
                if (g_zoomRestorePending) return;
            }
            InterlockedExchange(&g_zoomReadiness, 1);
            g_fovScan.result = FovScanResult::Idle;
            LogZoomStatus("target-invalidated");
        }
        if (g_fovScan.result == FovScanResult::Idle ||
            (g_fovScan.result != FovScanResult::Running && g_fovScan.result != FovScanResult::Verifying &&
             GetTickCount64() - g_fovScan.finishedAt >= kFovRetryDelayMs)) StartFovScan();
        AdvanceFovScan();
    }

    void UpdateOverlayBounds(HWND gameWindow)
    {
        RECT clientRect{};
        if (!GetClientRect(gameWindow, &clientRect))
        {
            return;
        }

        POINT topLeft{ clientRect.left, clientRect.top };
        POINT bottomRight{ clientRect.right, clientRect.bottom };
        if (!ClientToScreen(gameWindow, &topLeft) || !ClientToScreen(gameWindow, &bottomRight))
        {
            return;
        }

        const int width = bottomRight.x - topLeft.x;
        const int height = bottomRight.y - topLeft.y;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        SetWindowPos(
            g_overlayWindow,
            HWND_TOPMOST,
            topLeft.x,
            topLeft.y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }

    LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT clientRect{};
            GetClientRect(window, &clientRect);
            FillRect(dc, &clientRect, g_transparentBrush);

            RECT panel{ 22, 22, 920, 176 };
            HBRUSH panelBrush = CreateSolidBrush(kPanelColor);
            if (panelBrush != nullptr)
            {
                FillRect(dc, &panel, panelBrush);
                DeleteObject(panelBrush);
            }

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_overlayTextColor);
            HFONT font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ oldFont = font != nullptr ? SelectObject(dc, font) : nullptr;
            TextOutW(dc, 38, 36, g_overlayMessage, lstrlenW(g_overlayMessage));
            const LONG nametagReady = InterlockedCompareExchange(&nametag::readiness, 0, 0);
            const wchar_t* nametagText = nametagReady == 2 ?
                (InterlockedCompareExchange(&nametag::enabled, 0, 0) ?
                    L"Nametag: ON | both third-person views" : L"Nametag: OFF") :
                (nametagReady == 3 ? L"Nametag unavailable; see nametag diagnostic log" : L"Nametag preparing automatically");
            TextOutW(dc, 38, 70, nametagText, lstrlenW(nametagText));
            const wchar_t* dayText = InterlockedCompareExchange(&alwaysday::readiness, 0, 0) == 2 ?
                (InterlockedCompareExchange(&alwaysday::enabled, 0, 0) ? L"Always day: ON" : L"Always day: OFF") :
                L"Always day unavailable; see diagnostic log";
            TextOutW(dc, 38, 104, dayText, lstrlenW(dayText));
            const wchar_t* brightText = InterlockedCompareExchange(&fullbright::readiness, 0, 0) == 2 ?
                (InterlockedCompareExchange(&fullbright::enabled, 0, 0) ? L"Full Bright: ON" : L"Full Bright: OFF") :
                L"Full Bright unavailable; see diagnostic log";
            TextOutW(dc, 38, 138, brightText, lstrlenW(brightText));
            if (oldFont != nullptr)
            {
                SelectObject(dc, oldFont);
            }
            if (font != nullptr)
            {
                DeleteObject(font);
            }

            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    DWORD WINAPI OverlayWorker(void*)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = OverlayWindowProc;
        windowClass.hInstance = g_module;
        windowClass.lpszClassName = kWindowClassName;

        const ATOM registeredClass = RegisterClassExW(&windowClass);
        if (registeredClass == 0)
        {
            InterlockedExchange(&g_workerStarted, 0);
            FreeLibraryAndExitThread(g_module, 0);
        }

        g_transparentBrush = CreateSolidBrush(kTransparentColor);
        if (g_transparentBrush == nullptr)
        {
            UnregisterClassW(kWindowClassName, g_module);
            InterlockedExchange(&g_workerStarted, 0);
            FreeLibraryAndExitThread(g_module, 0);
        }

        g_overlayWindow = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kWindowClassName,
            L"GameMod local controls",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr,
            nullptr,
            g_module,
            nullptr);

        if (g_overlayWindow == nullptr ||
            !SetLayeredWindowAttributes(g_overlayWindow, kTransparentColor, 0, LWA_COLORKEY))
        {
            if (g_overlayWindow != nullptr)
            {
                DestroyWindow(g_overlayWindow);
                g_overlayWindow = nullptr;
            }
            DeleteObject(g_transparentBrush);
            g_transparentBrush = nullptr;
            UnregisterClassW(kWindowClassName, g_module);
            InterlockedExchange(&g_workerStarted, 0);
            FreeLibraryAndExitThread(g_module, 0);
        }

        nametag::Initialize(g_gameModuleBase, g_supportedBuild);
        LogNametagStatus("initialize");
        alwaysday::Initialize(g_gameModuleBase, g_supportedBuild);
        LogAlwaysDayStatus("initialize");
        fullbright::Initialize(g_gameModuleBase, g_supportedBuild);
        LogFullBrightStatus("initialize");
        ULONGLONG lastBrightLog = GetTickCount64();
        bool brightWasDown = (GetAsyncKeyState(g_zoomConfig.fullBrightKey) & 0x8000) != 0;
        ULONGLONG lastDayLog = GetTickCount64();
        bool dayWasDown = (GetAsyncKeyState(g_zoomConfig.alwaysDayKey) & 0x8000) != 0;
        LONG lastNametagReadiness = InterlockedCompareExchange(&nametag::readiness, 0, 0);
        ULONGLONG lastNametagLog = GetTickCount64();
        LONG64 lastOwnChecks = 0;
        bool f6WasDown = false;
        bool f7WasDown = (GetAsyncKeyState(g_zoomConfig.nametagKey) & 0x8000) != 0;
        bool indicatorWasDown = false;
        bool escapeWasDown = false;
        bool detachWasDown = false;
        bool zoomNeedsRelease = false;
        bool stopWhenRestored = false;
        ULONGLONG lastRawApiHookScanAt = 0;
        ULONGLONG lastGameInputHookScanAt = 0;
        bool rawInputHookStatusLogged = false;
        bool rawInputHookMissingLogged = false;
        MSG message{};

        g_trackedGameWindow = GetGameWindow();
        g_zoomKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL,
            ZoomKeyboardHookProc, g_module, 0);
        if (g_zoomKeyboardHook == nullptr)
        {
            LogZoomStatus("zoom-keyboard-hook-failed");
        }
        else
        {
            LogZoomStatus("zoom-keyboard-hooked");
        }

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    stopWhenRestored = true;
                    break;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (InterlockedCompareExchange(&g_stopRequested, 0, 0) != 0)
            {
                break;
            }

            const HWND gameWindow = GetGameWindow();
            g_trackedGameWindow = gameWindow;
            const ULONGLONG loopTime = GetTickCount64();
            if (loopTime - lastRawApiHookScanAt >= 2'000)
            {
                const unsigned int patched = InstallRawInputApiHooks();
                if (patched > 0)
                {
                    LogZoomStatus("raw-input-api-hooked");
                    rawInputHookStatusLogged = true;
                }
                else if (!rawInputHookStatusLogged)
                {
                    if (!rawInputHookMissingLogged)
                    {
                        LogZoomStatus("raw-input-api-hook-not-found-yet");
                        rawInputHookMissingLogged = true;
                    }
                }
                lastRawApiHookScanAt = loopTime;
            }
            if (loopTime - lastGameInputHookScanAt >= 2'000)
            {
                if (InstallGameInputApiHooks())
                {
                    LogZoomStatus("gameinput-api-hooked");
                }
                const unsigned int installed = InstallGameInputMessageHooks();
                if (installed > 0)
                {
                    LogZoomStatus("game-input-message-hooks-installed");
                }
                lastGameInputHookScanAt = loopTime;
            }
            const bool active = IsGameActive(gameWindow);

            if (active)
            {
                const bool f6Down = IsZoomKeyDown();
                if (zoomNeedsRelease && !f6Down)
                {
                    zoomNeedsRelease = false;
                }

                const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if (escapeDown && !escapeWasDown)
                {
                    ClearZoomWheelInput();
                    zoomNeedsRelease = f6Down;
                    if (g_zoom.active)
                    {
                        BeginZoomRestoreTransition();
                        SetOverlayMessage(L"Restoring original FOV", RGB(135, 255, 170));
                    }
                }
                escapeWasDown = escapeDown;

                if (f6Down && !f6WasDown && !stopWhenRestored)
                {
                    InterlockedExchange(&g_indicatorVisible, 1);
                    if (zoomNeedsRelease)
                    {
                    SetOverlayMessage(L"Release Zoom key after leaving or refocusing the game", RGB(255, 220, 120));
                    }
                    else if (g_zoomRestorePending)
                    {
                        SetOverlayMessage(L"Zoom restore pending; no new zoom started", RGB(255, 120, 120));
                    }
                    else
                    {
                        ApplyZoom(gameWindow);
                    }

                    UpdateOverlayBounds(gameWindow);
                    InvalidateRect(g_overlayWindow, nullptr, TRUE);
                }

                if (!f6Down && f6WasDown && g_zoom.active)
                {
                    BeginZoomRestoreTransition();
                    SetOverlayMessage(L"Restoring original FOV", RGB(135, 255, 170));
                }
                f6WasDown = f6Down;

                if (f6Down && g_zoom.active)
                {
                    ApplyZoomWheel(gameWindow, f6Down);
                }
                else
                {
                    ClearZoomWheelInput();
                }

                const bool brightDown = (GetAsyncKeyState(g_zoomConfig.fullBrightKey) & 0x8000) != 0;
                if (brightDown && !brightWasDown && !stopWhenRestored)
                {
                    const bool toggled = fullbright::Toggle();
                    LogFullBrightStatus(toggled ? "toggle" : "toggle-refused");
                    InterlockedExchange(&g_indicatorVisible, 1);
                    SetOverlayMessage(toggled ? L"Full Bright toggled" : L"Full Bright unavailable; profile refused",
                        toggled ? RGB(135, 255, 170) : RGB(255, 220, 120));
                }
                brightWasDown = brightDown;
                const bool dayDown = (GetAsyncKeyState(g_zoomConfig.alwaysDayKey) & 0x8000) != 0;
                if (dayDown && !dayWasDown && !stopWhenRestored)
                {
                    const bool toggled = alwaysday::Toggle();
                    LogAlwaysDayStatus(toggled ? "toggle" : "toggle-refused");
                    InterlockedExchange(&g_indicatorVisible, 1);
                    SetOverlayMessage(toggled ? L"Always day toggled" : L"Always day unavailable; profile refused",
                        toggled ? RGB(135, 255, 170) : RGB(255, 220, 120));
                }
                dayWasDown = dayDown;
                const bool f7Down = (GetAsyncKeyState(g_zoomConfig.nametagKey) & 0x8000) != 0;
                if (f7Down && !f7WasDown && !stopWhenRestored)
                {
                    const bool toggled = nametag::Toggle();
                    LogNametagStatus(toggled ? "toggle" : "toggle-refused");
                    InterlockedExchange(&g_indicatorVisible, 1);
                    SetOverlayMessage(toggled ? L"Nametag toggled" :
                        L"Nametag is not ready; no name enabled", toggled ? RGB(135, 255, 170) : RGB(255, 220, 120));
                    UpdateOverlayBounds(gameWindow);
                }
                f7WasDown = f7Down;

                const bool indicatorDown = (GetAsyncKeyState(g_zoomConfig.indicatorKey) & 0x8000) != 0;
                if (indicatorDown && !indicatorWasDown)
                {
                    const LONG newVisibility = InterlockedCompareExchange(&g_indicatorVisible, 0, 0) == 0 ? 1 : 0;
                    InterlockedExchange(&g_indicatorVisible, newVisibility);
                    if (newVisibility != 0)
                    {
                        UpdateOverlayBounds(gameWindow);
                        InvalidateRect(g_overlayWindow, nullptr, TRUE);
                    }
                    else
                    {
                        ShowWindow(g_overlayWindow, SW_HIDE);
                    }
                }
                indicatorWasDown = indicatorDown;

                const bool detachDown = (GetAsyncKeyState(g_zoomConfig.exitKey) & 0x8000) != 0;
                if (detachDown && !detachWasDown)
                {
                    if (!g_zoom.active)
                    {
                        InterlockedExchange(&g_stopRequested, 1);
                    }
                    else
                    {
                        stopWhenRestored = true;
                        BeginZoomRestoreTransition();
                        InterlockedExchange(&g_indicatorVisible, 1);
                        SetOverlayMessage(L"Restoring FOV before module exit", RGB(255, 220, 120));
                        UpdateOverlayBounds(gameWindow);
                    }
                }
                detachWasDown = detachDown;

                if (InterlockedCompareExchange(&g_indicatorVisible, 0, 0) != 0)
                {
                    UpdateOverlayBounds(gameWindow);
                    InvalidateRect(g_overlayWindow, nullptr, FALSE);
                }
            }
            else
            {
                ClearZoomWheelInput();
                const bool f6Down = IsZoomKeyDown();
                zoomNeedsRelease = f6Down;
                f6WasDown = f6Down;
                if (g_zoom.active)
                {
                    BeginZoomRestoreTransition();
                    SetOverlayMessage(L"Restoring original FOV after focus loss", RGB(135, 255, 170));
                }

                indicatorWasDown = false;
                brightWasDown = (GetAsyncKeyState(g_zoomConfig.fullBrightKey) & 0x8000) != 0;
                dayWasDown = (GetAsyncKeyState(g_zoomConfig.alwaysDayKey) & 0x8000) != 0;
                f7WasDown = (GetAsyncKeyState(g_zoomConfig.nametagKey) & 0x8000) != 0;
                escapeWasDown = false;
                detachWasDown = false;
                if (IsWindowVisible(g_overlayWindow))
                {
                    ShowWindow(g_overlayWindow, SW_HIDE);
                }
            }

            if (active && g_zoomRestorePending && g_zoom.active)
            {
                const FovRestoreResult restore = RestoreZoom();
                if (restore == FovRestoreResult::Restored)
                {
                    SetOverlayMessage(L"Original FOV restored after retry", RGB(135, 255, 170));
                    if (stopWhenRestored)
                    {
                        InterlockedExchange(&g_stopRequested, 1);
                    }
                }
                else if (restore == FovRestoreResult::ValueChanged)
                {
                    SetOverlayMessage(L"FOV changed elsewhere; value left untouched", RGB(255, 220, 120));
                    if (stopWhenRestored)
                    {
                        InterlockedExchange(&g_stopRequested, 1);
                    }
                }
            }

            AdvanceZoomTransition();

            if ((stopWhenRestored || InterlockedCompareExchange(&g_stopRequested, 0, 0)) &&
                !g_zoom.transitionActive)
            {
                stopWhenRestored = true;
                const FovRestoreResult restore = RestoreZoom();
                const bool brightRestored = fullbright::Shutdown();
                const bool dayRestored = alwaysday::Shutdown();
                const bool nametagRestored = nametag::Shutdown();
                if (restore != FovRestoreResult::Failed && nametagRestored && dayRestored && brightRestored)
                {
                    LogNametagStatus("exit-restored");
                    LogAlwaysDayStatus("exit-restored");
                    LogFullBrightStatus("exit-restored");
                    InterlockedExchange(&g_stopRequested, 1);
                }
                else
                {
                    InterlockedExchange(&g_stopRequested, 0);
                    SetOverlayMessage(L"Restoring native state; module kept active until safe", RGB(255, 120, 120));
                }
            }

            if (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && !stopWhenRestored)
            {
                if (GetTickCount64() - lastBrightLog >= 5'000 && InterlockedCompareExchange(&fullbright::enabled, 0, 0))
                {
                    LogFullBrightStatus("runtime");
                    lastBrightLog = GetTickCount64();
                }
                if (GetTickCount64() - lastDayLog >= 5'000 && InterlockedCompareExchange(&alwaysday::enabled, 0, 0))
                {
                    LogAlwaysDayStatus("runtime");
                    lastDayLog = GetTickCount64();
                }
                UpdateFovDiscovery();
                nametag::BindOptions(g_fovScan.result == FovScanResult::Unique ? g_fovScan.foundTarget.options : 0);
                const LONG ready = InterlockedCompareExchange(&nametag::readiness, 0, 0);
                const LONG64 checks = InterlockedCompareExchange64(&nametag::ownChecks, 0, 0);
                if (ready != lastNametagReadiness || (checks != lastOwnChecks && GetTickCount64() - lastNametagLog >= 5'000))
                {
                    LogNametagStatus("runtime");
                    lastNametagReadiness = ready;
                    lastOwnChecks = checks;
                    lastNametagLog = GetTickCount64();
                }
                if (!g_zoom.active && fov::MayActivate(active,
                    IsZoomKeyDown(),
                    (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0,
                    zoomNeedsRelease, g_zoomRestorePending, stopWhenRestored,
                    g_fovScan.result == FovScanResult::Unique))
                {
                    ApplyZoom(gameWindow);
                }
            }
            Sleep(g_fovScan.result == FovScanResult::Running ? 1 : 16);
        }

        for (const GameInputHook& hook : g_gameInputHooks)
        {
            if (hook.handle != nullptr) UnhookWindowsHookEx(hook.handle);
        }
        g_gameInputHooks.clear();
        bool inputRestorePendingLogged = false;
        while (!RestoreNativeMouseWheelHooks())
        {
            if (!inputRestorePendingLogged)
            {
                LogZoomStatus("native-mouse-wheel-restore-pending");
                inputRestorePendingLogged = true;
            }
            SetOverlayMessage(L"Input hook restore pending; module remains loaded", RGB(255, 170, 110));
            MSG restoreMessage{};
            while (PeekMessageW(&restoreMessage, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&restoreMessage);
                DispatchMessageW(&restoreMessage);
            }
            Sleep(16);
        }
        RestoreGameInputHooks();
        if (g_zoomKeyboardHook != nullptr)
        {
            UnhookWindowsHookEx(g_zoomKeyboardHook);
            g_zoomKeyboardHook = nullptr;
        }
        InterlockedExchange(&g_zoomKeyDown, 0);
        g_trackedGameWindow = nullptr;
        InterlockedExchange(&g_rawWheelInputActive, 0);
        InterlockedExchange(&g_lastLegacyWheelDelta, 0);
        InterlockedExchange64(&g_lastLegacyWheelAt, 0);
        ClearZoomWheelInput();

        if (g_overlayWindow != nullptr)
        {
            DestroyWindow(g_overlayWindow);
            g_overlayWindow = nullptr;
        }
        DeleteObject(g_transparentBrush);
        g_transparentBrush = nullptr;
        UnregisterClassW(kWindowClassName, g_module);
        InterlockedExchange(&g_workerStarted, 0);
        FreeLibraryAndExitThread(g_module, 0);
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI StartGameMod(LPVOID)
{
    if (InterlockedCompareExchange(&g_workerStarted, 1, 0) != 0)
    {
        return 2; // Already running: do not reset the discovery or active Zoom.
    }

    InterlockedExchange(&g_stopRequested, 0);
    InterlockedExchange(&g_indicatorVisible, 0);
    InterlockedExchange(&g_zoomMouseScalingActive, 0);
    InterlockedExchange(&g_zoomMouseScaleBasisPoints, 10'000);
    InterlockedExchange(&g_nativeMouseCallbacks, 0);
    std::fill(std::begin(g_nativeMouseHookStates), std::end(g_nativeMouseHookStates), NativeMouseHookState{});
    InterlockedExchange(&g_zoomInputEpoch, 0);
    std::fill(std::begin(g_mouseInputTraces), std::end(g_mouseInputTraces), MouseInputTrace{});
    g_mouseInputTraceCount = 0;
    g_mouseInputTraceLines = 0;
    std::fill(std::begin(g_gameInputMousePositionScale), std::end(g_gameInputMousePositionScale),
        PerDeviceMousePositionScaleState{});
    g_zoom = {};
    g_zoomSensitivity = {};
    g_zoomConfig = {};
    g_zoomRestorePending = false;
    g_fovPointerConfig = {};
    g_fovScan = {};
    g_supportedBuild = false;
    g_gameModuleBase = 0;
    InterlockedExchange(&g_zoomReadiness, 0);
    InterlockedExchange(&nametag::readiness, 0);
    InterlockedExchange(&alwaysday::readiness, 0);
    InterlockedExchange(&fullbright::readiness, 0);
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, path, MAX_PATH) > 0)
    {
        g_statusLogPath = path;
        const size_t separator = g_statusLogPath.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            g_statusLogPath.resize(separator + 1);
            g_statusLogPath += L"zoom-status-" + std::to_wstring(GetCurrentProcessId()) + L".log";
            g_nametagLogPath = g_statusLogPath.substr(0, separator + 1) +
                L"nametag-status-" + std::to_wstring(GetCurrentProcessId()) + L".log";
            g_alwaysDayLogPath = g_statusLogPath.substr(0, separator + 1) +
                L"always-day-status-" + std::to_wstring(GetCurrentProcessId()) + L".log";
            g_fullBrightLogPath = g_statusLogPath.substr(0, separator + 1) +
                L"full-bright-status-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        }
        else g_statusLogPath.clear();
    }
    g_fovScan.startedAt = GetTickCount64();
    if (!LoadZoomConfig())
    {
        g_zoomConfig = {};
        LogZoomStatus("zoom-config-invalid-using-defaults");
    }
    else
    {
        LogZoomStatus("zoom-config-loaded");
    }
    if (!InitializeFovBuild())
    {
        InterlockedExchange(&g_zoomReadiness, 3);
        LogZoomStatus("unsupported-build");
        SetOverlayMessage(L"Unsupported Minecraft build; Zoom will not change memory", RGB(255, 170, 110));
    }
    else
    {
        LoadFovPointerConfig();
        fov::Target hint;
        const FovResolveResult result = ResolveFovTarget(hint);
        LogZoomStatus(result == FovResolveResult::Resolved ? "chain-verified-needs-uniqueness" : "chain-rejected");
        // The saved chain is still tried first, but historical allocator paths
        // never bypass the global uniqueness check for the actual Options type.
        StartFovScan();
    }
    const HANDLE worker = CreateThread(nullptr, 0, OverlayWorker, nullptr, 0, nullptr);
    if (worker == nullptr)
    {
        InterlockedExchange(&g_workerStarted, 0);
        return 0;
    }

    CloseHandle(worker);
    return 1;
}

extern "C" __declspec(dllexport) DWORD WINAPI GetZoomReadiness(LPVOID)
{
    return static_cast<DWORD>(InterlockedCompareExchange(&g_zoomReadiness, 0, 0));
}

extern "C" __declspec(dllexport) DWORD WINAPI GetAlwaysDayReadiness(LPVOID)
{
    return static_cast<DWORD>(InterlockedCompareExchange(&alwaysday::readiness, 0, 0));
}

extern "C" __declspec(dllexport) DWORD WINAPI GetFullBrightReadiness(LPVOID)
{
    return static_cast<DWORD>(InterlockedCompareExchange(&fullbright::readiness, 0, 0));
}

extern "C" __declspec(dllexport) DWORD WINAPI GetNametagReadiness(LPVOID)
{
    return static_cast<DWORD>(InterlockedCompareExchange(&nametag::readiness, 0, 0));
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        // Nametag uses POD thread-local render context; allow CRT TLS handling.
    }

    return TRUE;
}
