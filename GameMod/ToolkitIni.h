#pragma once

#include <windows.h>
#include <climits>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace toolkitini
{
    inline std::wstring TrimWhitespace(const std::wstring& text)
    {
        const size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return {};
        }

        const size_t last = text.find_last_not_of(L" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    inline bool ReadUtf8BomConfig(const std::wstring& path, std::wstring& config)
    {
        std::ifstream file(path, std::ios::binary);
        char bom[3]{};
        if (!file.read(bom, sizeof(bom)) || std::memcmp(bom, "\xEF\xBB\xBF", sizeof(bom)) != 0)
            return true;

        const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (bytes.empty()) return true;
        if (bytes.size() > INT_MAX) return false;
        const int size = static_cast<int>(bytes.size());
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
        if (length == 0) return false;
        config.resize(length);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, config.data(), length) == length;
    }

    inline bool ReadDecodedConfigValue(const std::wstring& config, const wchar_t* section, const wchar_t* key,
        std::wstring& value)
    {
        std::wistringstream stream(config);
        bool inSection = false;
        for (std::wstring line; std::getline(stream, line);)
        {
            line = TrimWhitespace(line);
            if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
            if (line.front() == L'[' && line.back() == L']')
            {
                inSection = _wcsicmp(TrimWhitespace(line.substr(1, line.size() - 2)).c_str(), section) == 0;
                continue;
            }
            const size_t separator = line.find(L'=');
            if (inSection && separator != std::wstring::npos &&
                _wcsicmp(TrimWhitespace(line.substr(0, separator)).c_str(), key) == 0)
            {
                value = TrimWhitespace(line.substr(separator + 1));
                return true;
            }
        }
        return false;
    }
}
