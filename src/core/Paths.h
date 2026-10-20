#pragma once

#include "PCH.h"

namespace stm::paths
{
    // The game root - the directory that holds Data/.
    //
    // Every path this plugin uses is relative to it, and the process working
    // directory is the right answer (MO2 starts the game from there). But
    // std::filesystem::current_path() *throws* when it cannot be determined,
    // and these lookups happen inside render callbacks and function-local
    // static initialisers, where an exception means std::terminate. So it is
    // queried with an error_code and falls back to the directory the DLL lives
    // in: <root>/SKSE/Plugins/SlaveTatsMenu.dll -> <root>.
    inline const std::filesystem::path& GameRoot()
    {
        static const std::filesystem::path s_root = [] {
            std::error_code ec;
            auto            cwd = std::filesystem::current_path(ec);
            if (!ec && !cwd.empty()) {
                return cwd;
            }

            HMODULE module = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&GameRoot), &module) &&
                module) {
                wchar_t buffer[MAX_PATH]{};
                if (::GetModuleFileNameW(module, buffer, MAX_PATH)) {
                    // .../Data/SKSE/Plugins/<dll> -> three levels up.
                    auto root = std::filesystem::path(buffer).parent_path().parent_path().parent_path();
                    if (!root.empty()) {
                        return root;
                    }
                }
            }

            // Nothing usable: a relative path at least keeps the plugin running.
            return std::filesystem::path(".");
        }();
        return s_root;
    }
}
