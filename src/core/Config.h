#pragma once

#include "PCH.h"

namespace stm
{
    enum class Language
    {
        kChinese,
        kEnglish,
    };

    enum class SlotPolicy
    {
        kAutoFreeSlot = 0,  // 自动顺延至空槽位
        kOverwrite = 1      // 覆盖目标槽位
    };

    enum class MannequinColor
    {
        kTransparent = 0,  // 默认透明全息
        kFlesh = 1         // 自然肉色
    };

    struct Config
    {
        // [General]
        std::uint32_t toggleKey = 0x44;  // DirectInput scan code, 0x44 = F10
        float         defaultAlpha = 1.0f;
        std::uint32_t defaultColor = 0x000000;
        Language      language = Language::kEnglish;
        SlotPolicy    slotPolicy = SlotPolicy::kAutoFreeSlot;

        // [UI]
        float windowWidth = 1280.0f;
        float windowHeight = 720.0f;
        float windowAlpha = 0.95f;   // 0.2 ~ 1.0
        float fontScale = 1.0f;     // 0.75 ~ 1.5
        bool  showPreview = true;   // 是否显示图案预览
        float previewSize = 96.0f;  // 预览图边长
        MannequinColor mannequinColor = MannequinColor::kFlesh; // 3D模特人体颜色风格
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
