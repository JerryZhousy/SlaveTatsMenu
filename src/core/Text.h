#pragma once

#include "PCH.h"

// UTF-8 <-> UTF-16 helpers.
//
// std::filesystem::path::string() converts through the active ANSI code page, so
// a UTF-8 preset name containing CJK would turn to mojibake on the way to a file
// name. Anything user-visible that becomes a path goes through here instead.

namespace stm::text
{
    inline std::wstring Utf8ToWide(std::string_view a_utf8)
    {
        if (a_utf8.empty()) {
            return {};
        }

        const int length = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
        if (length <= 0) {
            return {};
        }

        std::wstring result(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), result.data(), length);
        return result;
    }

    inline std::string WideToUtf8(std::wstring_view a_wide)
    {
        if (a_wide.empty()) {
            return {};
        }

        const int length = ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            return {};
        }

        std::string result(static_cast<std::size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), result.data(), length, nullptr, nullptr);
        return result;
    }
}
