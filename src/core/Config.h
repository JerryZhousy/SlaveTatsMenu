#pragma once

#include "PCH.h"

namespace stm
{
    enum class Language
    {
        kChinese,
        kEnglish,
    };

    struct Config
    {
        // [General]
        std::uint32_t toggleKey = 0x44;  // DirectInput scan code, 0x44 = F10
        float         defaultAlpha = 1.0f;
        std::uint32_t defaultColor = 0x000000;
        Language      language = Language::kEnglish;

        // [UI]
        float windowWidth = 1280.0f;
        float windowHeight = 720.0f;
        int   slotRefreshMs = 250;  // how often the applied-slot snapshot is rebuilt

        // [Debug]
        bool        probeEnabled = false;
        int         probeIntervalMs = 1000;
        std::string probeArea = "Body";
        int         probeSlots = 12;

        // Wraps the mutating entries of SlaveTatsNG's interface table so every
        // C++ caller is logged by name (see diag/CallTrace.h). On by default: it
        // is the only way to attribute a silent ledger change to a plugin.
        bool callTraceEnabled = true;

        static Config& Get();

        // Reads the INI; writes a default one if it is missing or unreadable.
        void Load();
        void Save();

        bool IsChinese() const { return language == Language::kChinese; }

        // Absolute path of Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini.
        // Uses the process working directory (the game root under MO2) so the
        // virtualised Data folder is resolved correctly.
        static std::filesystem::path IniPath();
    };
}
