#pragma once

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string>

namespace fpscounter
{
    using StatusLogger = void(*)(const char*);
    constexpr wchar_t kWindowClass[] = L"BedrockToolkit.FpsCounter";
    constexpr BYTE kBackgroundAlpha = 166; // 65% opacity, rounded to an 8-bit alpha.
    constexpr int kDefaultFontSize = 28;
    constexpr int kMinimumFontSize = 12;
    constexpr int kMaximumFontSize = 48;

    inline bool LoadEnabled(const std::wstring& path)
    {
        wchar_t value[16]{};
        const DWORD length = GetPrivateProfileStringW(L"fps_counter", L"enabled", L"",
            value, static_cast<DWORD>(std::size(value)), path.c_str());
        if (length == 0)
        {
            wchar_t keys[128]{};
            const DWORD keyLength = GetPrivateProfileStringW(L"fps_counter", nullptr, L"",
                keys, static_cast<DWORD>(std::size(keys)), path.c_str());
            for (const wchar_t* key = keys; key < keys + keyLength; key += lstrlenW(key) + 1)
                if (_wcsicmp(key, L"enabled") == 0) return false;
            return true;
        }
        if (length >= std::size(value) - 1) return false;
        const std::wstring text(value, length);
        const size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return false;
        const size_t last = text.find_last_not_of(L" \t\r\n");
        return _wcsicmp(text.substr(first, last - first + 1).c_str(), L"true") == 0;
    }

    inline int LoadFontSize(const std::wstring& path)
    {
        wchar_t value[16]{};
        const DWORD length = GetPrivateProfileStringW(L"fps_counter", L"font_size", L"28",
            value, static_cast<DWORD>(std::size(value)), path.c_str());
        if (length == 0 || length >= std::size(value) - 1) return kDefaultFontSize;
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value, &end, 10);
        while (end != nullptr && (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n')) ++end;
        return end == value || end == nullptr || *end != L'\0' ||
            parsed < kMinimumFontSize || parsed > kMaximumFontSize ? kDefaultFontSize : static_cast<int>(parsed);
    }

    inline int FpsValue(uint64_t frames, ULONGLONG elapsedMs)
    {
        if (elapsedMs == 0) return -1;
        const double fps = static_cast<double>(frames) * 1000.0 / static_cast<double>(elapsedMs);
        return std::isfinite(fps) && fps >= 0.0 ?
            static_cast<int>(std::lround((std::min)(fps, 1000.0))) : -1;
    }

    inline RECT PanelRect(const RECT& client, SIZE_T width, SIZE_T height)
    {
        constexpr LONG margin = 8;
        const LONG left = client.left + margin;
        const LONG bottom = client.bottom - margin;
        return {left, bottom - static_cast<LONG>(height), left + static_cast<LONG>(width), bottom};
    }

    inline bool IsGameForegroundWindow(HWND foreground, HWND game)
    {
        if (foreground == nullptr || game == nullptr) return false;

        DWORD foregroundProcessId = 0;
        DWORD gameProcessId = 0;
        if (GetWindowThreadProcessId(foreground, &foregroundProcessId) == 0 ||
            GetWindowThreadProcessId(game, &gameProcessId) == 0) return false;
        if (foregroundProcessId == gameProcessId) return true;

        const HWND foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
        return foregroundRoot == game || IsChild(foregroundRoot, game) || IsChild(game, foregroundRoot);
    }

    inline uint32_t Pixel(BYTE coverage)
    {
        // White text over a translucent black panel, in premultiplied BGRA.
        const uint32_t alpha = kBackgroundAlpha + (255 - kBackgroundAlpha) * coverage / 255;
        return (alpha << 24) | (uint32_t(coverage) * 0x010101);
    }

    class Counter
    {
    public:
        bool Start(HMODULE module, const std::wstring& configPath, StatusLogger logger = nullptr)
        {
            module_ = module;
            configPath_ = configPath;
            logger_ = logger;
            stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (stop_ == nullptr) return false;
            thread_ = CreateThread(nullptr, 0, Run, this, 0, nullptr);
            if (thread_ != nullptr) return true;
            CloseHandle(stop_);
            stop_ = nullptr;
            return false;
        }

        bool Enabled() const { return enabled_.load(); }
        HWND Window() const { return window_.load(); }

        void NoteFrame()
        {
            frames_.fetch_add(1, std::memory_order_relaxed);
        }

        void Publish(HWND gameWindow, bool gameplayActive, bool frameSourceReady)
        {
            gameWindow_.store(gameWindow);
            gameplayActive_.store(gameplayActive && frameSourceReady);
        }

        void BeginShutdown()
        {
            enabled_.store(false);
            if (stop_ != nullptr) SetEvent(stop_);
        }

        bool FinishShutdown()
        {
            if (thread_ == nullptr) return true;
            if (WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) return false;
            CloseHandle(thread_);
            CloseHandle(stop_);
            thread_ = nullptr;
            stop_ = nullptr;
            return true;
        }

    private:
        void ReportStatus(const char* event)
        {
            if (logger_ != nullptr) logger_(event);
        }

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCHITTEST) return HTTRANSPARENT;
            if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            if (message == WM_CLOSE) return 0;
            return DefWindowProcW(window, message, wParam, lParam);
        }

        HFONT CreateCounterFont(HDC dc, int fontSize)
        {
            for (const wchar_t* face : {L"Minecraftia", L"Mojangles"})
            {
                HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                    DEFAULT_PITCH, face);
                if (font == nullptr) continue;
                HGDIOBJ old = SelectObject(dc, font);
                wchar_t actual[LF_FACESIZE]{};
                GetTextFaceW(dc, LF_FACESIZE, actual);
                SelectObject(dc, old);
                if (_wcsicmp(actual, face) == 0) return font;
                DeleteObject(font);
            }
            return nullptr;
        }

        void LoadGameFont()
        {
            wchar_t path[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return;
            const std::wstring gamePath(path, length);
            const size_t separator = gamePath.find_last_of(L"\\/");
            if (separator == std::wstring::npos) return;
            const std::wstring fontPath = gamePath.substr(0, separator + 1) + L"data\\fonts\\Mojangles.ttf";
            if (AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) != 0) fontPath_ = fontPath;
        }

        bool Draw(HWND window, HDC dc, HFONT font, const RECT& client, int value)
        {
            wchar_t text[16]{};
            swprintf_s(text, L"FPS: %d", value);
            HGDIOBJ oldFont = SelectObject(dc, font);
            SIZE extent{};
            if (!GetTextExtentPoint32W(dc, text, lstrlenW(text), &extent))
            {
                SelectObject(dc, oldFont);
                return false;
            }
            const LONG width = extent.cx + 16;
            const LONG height = extent.cy + 12;
            const RECT panel = PanelRect(client, width, height);
            if (panel.top < client.top || panel.right > client.right)
            {
                SelectObject(dc, oldFont);
                return false;
            }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            bool drawn = false;
            if (bitmap != nullptr)
            {
                HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
                PatBlt(dc, 0, 0, width, height, BLACKNESS);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                const bool textDrawn = TextOutW(dc, 8, 6, text, lstrlenW(text)) != FALSE;
                GdiFlush();
                auto* pixels = static_cast<uint32_t*>(bits);
                for (LONG i = 0; i < width * height; ++i)
                    pixels[i] = Pixel(static_cast<BYTE>(pixels[i] & 0xFF));
                POINT destination{panel.left, panel.top};
                POINT source{};
                SIZE size{width, height};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                drawn = textDrawn && UpdateLayeredWindow(window, nullptr, &destination, &size,
                    dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
                SelectObject(dc, oldBitmap);
                DeleteObject(bitmap);
            }
            SelectObject(dc, oldFont);
            return drawn;
        }

        static DWORD WINAPI Run(void* context)
        {
            static_cast<Counter*>(context)->Run();
            return 0;
        }

        void Run()
        {
            HWND initialGameWindow = nullptr;
            while ((initialGameWindow = gameWindow_.load()) == nullptr || !IsWindow(initialGameWindow))
            {
                if (WaitForSingleObject(stop_, 16) != WAIT_TIMEOUT) return;
            }

            // The counter has its own thread, so it does not inherit the game
            // window's thread DPI context. Use the target window's context
            // before creating the popup and converting client coordinates to
            // screen coordinates; otherwise Windows may virtualize those
            // coordinates and place the layered window away from the game.
            const DPI_AWARENESS_CONTEXT gameDpiContext = GetWindowDpiAwarenessContext(initialGameWindow);
            if (gameDpiContext == nullptr)
            {
                ReportStatus("fps-overlay-dpi-context-failed");
                return;
            }
            const DPI_AWARENESS_CONTEXT previousDpiContext =
                SetThreadDpiAwarenessContext(gameDpiContext);
            if (previousDpiContext == nullptr)
            {
                ReportStatus("fps-overlay-dpi-context-failed");
                return;
            }

            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.hInstance = module_;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.lpszClassName = kWindowClass;
            if (RegisterClassExW(&windowClass) == 0)
            {
                ReportStatus("fps-overlay-window-class-failed");
                SetThreadDpiAwarenessContext(previousDpiContext);
                return;
            }
            HWND window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kWindowClass, L"FPS counter", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_, nullptr);
            if (window == nullptr)
            {
                ReportStatus("fps-overlay-window-failed");
                UnregisterClassW(kWindowClass, module_);
                SetThreadDpiAwarenessContext(previousDpiContext);
                return;
            }
            window_.store(window);
            HDC dc = CreateCompatibleDC(nullptr);
            HFONT font = nullptr;
            ULONGLONG lastConfigRead = 0;
            ULONGLONG lastFrameSample = 0;
            uint64_t previousFrameCount = 0;
            int fontSize = kDefaultFontSize;
            bool fontConfigurationRead = false;
            RECT lastClient{};
            int lastValue = -1;
            int value = -1;
            bool fontStatusKnown = false;
            bool fontWasAvailable = false;
            bool sampleStatusReported = false;
            bool drawFailureReported = false;
            while (window != nullptr && dc != nullptr && WaitForSingleObject(stop_, 16) == WAIT_TIMEOUT)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                const ULONGLONG now = GetTickCount64();
                if (lastConfigRead == 0 || now - lastConfigRead >= 500)
                {
                    enabled_.store(LoadEnabled(configPath_));
                    const int nextFontSize = LoadFontSize(configPath_);
                    if (!fontConfigurationRead || nextFontSize != fontSize)
                    {
                        if (font != nullptr) DeleteObject(font);
                        font = nullptr;
                        fontSize = nextFontSize;
                        fontConfigurationRead = true;
                        lastValue = -1;
                    }
                    if (enabled_.load() && font == nullptr)
                    {
                        if (fontPath_.empty()) LoadGameFont();
                        font = CreateCounterFont(dc, fontSize);
                    }
                    const bool fontAvailable = font != nullptr;
                    if (!fontStatusKnown || fontAvailable != fontWasAvailable)
                    {
                        ReportStatus(fontAvailable ? "fps-font-ready" : "fps-font-unavailable");
                        fontWasAvailable = fontAvailable;
                        fontStatusKnown = true;
                    }
                    lastConfigRead = now;
                }
                const HWND game = gameWindow_.load();
                const bool gameplayActive = gameplayActive_.load() && enabled_.load();
                const uint64_t frameCount = frames_.load(std::memory_order_relaxed);
                if (!gameplayActive)
                {
                    previousFrameCount = frameCount;
                    lastFrameSample = now;
                    value = -1;
                }
                else if (lastFrameSample == 0)
                {
                    previousFrameCount = frameCount;
                    lastFrameSample = now;
                }
                else if (now - lastFrameSample >= 1'000)
                {
                    value = FpsValue(frameCount - previousFrameCount, now - lastFrameSample);
                    if (value >= 0 && !sampleStatusReported)
                    {
                        ReportStatus("fps-sample-ready");
                        sampleStatusReported = true;
                    }
                    previousFrameCount = frameCount;
                    lastFrameSample = now;
                }
                const HWND foreground = GetForegroundWindow();
                RECT client{};
                POINT origin{};
                const bool visible = gameplayActive && font != nullptr && value >= 0 &&
                    game != nullptr && IsWindowVisible(game) && !IsIconic(game) &&
                    IsGameForegroundWindow(foreground, game) &&
                    GetClientRect(game, &client) &&
                    ClientToScreen(game, &origin);
                if (visible)
                {
                    OffsetRect(&client, origin.x, origin.y);
                    if (value != lastValue || !EqualRect(&client, &lastClient))
                    {
                        if (!Draw(window, dc, font, client, value))
                        {
                            if (!drawFailureReported)
                            {
                                ReportStatus("fps-overlay-draw-failed");
                                drawFailureReported = true;
                            }
                            ShowWindow(window, SW_HIDE);
                            lastValue = -1;
                            continue;
                        }
                        lastValue = value;
                        lastClient = client;
                    }
                    if (!IsWindowVisible(window))
                        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
                else ShowWindow(window, SW_HIDE);
            }
            enabled_.store(false);
            if (window != nullptr) DestroyWindow(window);
            window_.store(nullptr);
            if (font != nullptr) DeleteObject(font);
            if (dc != nullptr) DeleteDC(dc);
            if (!fontPath_.empty()) RemoveFontResourceExW(fontPath_.c_str(), FR_PRIVATE, nullptr);
            UnregisterClassW(kWindowClass, module_);
            SetThreadDpiAwarenessContext(previousDpiContext);
        }

        HMODULE module_ = nullptr;
        HANDLE stop_ = nullptr;
        HANDLE thread_ = nullptr;
        std::wstring configPath_;
        std::wstring fontPath_;
        StatusLogger logger_ = nullptr;
        std::atomic<bool> enabled_{false};
        std::atomic<bool> gameplayActive_{false};
        std::atomic<HWND> gameWindow_{nullptr};
        std::atomic<HWND> window_{nullptr};
        std::atomic<uint64_t> frames_{0};
    };
}
