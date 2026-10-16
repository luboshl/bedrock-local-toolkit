#pragma once

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>

namespace fovcounter
{
    constexpr wchar_t kWindowClass[] = L"BedrockToolkit.FovCounter";
    constexpr BYTE kBackgroundAlpha = 166; // 65% opacity, rounded to an 8-bit alpha.

    inline bool LoadEnabled(const std::wstring& path)
    {
        wchar_t value[16]{};
        const DWORD length = GetPrivateProfileStringW(L"fov_counter", L"enabled", L"false",
            value, static_cast<DWORD>(std::size(value)), path.c_str());
        if (length == 0 || length >= std::size(value) - 1) return false;
        const std::wstring text(value, length);
        const size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return false;
        const size_t last = text.find_last_not_of(L" \t\r\n");
        return _wcsicmp(text.substr(first, last - first + 1).c_str(), L"true") == 0;
    }

    inline int DisplayValue(float value)
    {
        return std::isfinite(value) && value >= 1.0f && value <= 120.0f ?
            static_cast<int>(std::lround(value)) : -1;
    }

    inline RECT PanelRect(const RECT& client, SIZE_T width, SIZE_T height)
    {
        constexpr LONG margin = 8;
        const LONG left = client.left + margin;
        const LONG bottom = client.bottom - margin;
        return {left, bottom - static_cast<LONG>(height), left + static_cast<LONG>(width), bottom};
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
        bool Start(HMODULE module, const std::wstring& configPath)
        {
            module_ = module;
            configPath_ = configPath;
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

        void Publish(HWND gameWindow, int value)
        {
            gameWindow_.store(gameWindow);
            value_.store(value);
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
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCHITTEST) return HTTRANSPARENT;
            if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            if (message == WM_CLOSE) return 0;
            return DefWindowProcW(window, message, wParam, lParam);
        }

        HFONT CreateCounterFont(HDC dc)
        {
            for (const wchar_t* face : {L"Minecraftia", L"Mojangles"})
            {
                HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
            swprintf_s(text, L"FOV: %d", value);
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
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.hInstance = module_;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.lpszClassName = kWindowClass;
            if (RegisterClassExW(&windowClass) == 0) return;
            HWND window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kWindowClass, L"FOV counter", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_, nullptr);
            window_.store(window);
            HDC dc = CreateCompatibleDC(nullptr);
            HFONT font = nullptr;
            ULONGLONG lastConfigRead = 0;
            RECT lastClient{};
            int lastValue = -1;
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
                    if (enabled_.load() && font == nullptr)
                    {
                        if (fontPath_.empty()) LoadGameFont();
                        font = CreateCounterFont(dc);
                    }
                    lastConfigRead = now;
                }
                const HWND game = gameWindow_.load();
                const int value = value_.load();
                const HWND foreground = GetForegroundWindow();
                const HWND foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
                RECT client{};
                POINT origin{};
                const bool visible = enabled_.load() && font != nullptr && value >= 0 &&
                    game != nullptr && IsWindowVisible(game) && !IsIconic(game) &&
                    (foreground == game || foregroundRoot == game ||
                        IsChild(foregroundRoot, game) || IsChild(game, foregroundRoot)) &&
                    GetClientRect(game, &client) &&
                    ClientToScreen(game, &origin);
                if (visible)
                {
                    OffsetRect(&client, origin.x, origin.y);
                    if (value != lastValue || !EqualRect(&client, &lastClient))
                    {
                        if (!Draw(window, dc, font, client, value))
                        {
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
        }

        HMODULE module_ = nullptr;
        HANDLE stop_ = nullptr;
        HANDLE thread_ = nullptr;
        std::wstring configPath_;
        std::wstring fontPath_;
        std::atomic<bool> enabled_{false};
        std::atomic<HWND> gameWindow_{nullptr};
        std::atomic<HWND> window_{nullptr};
        std::atomic<int> value_{-1};
    };
}
