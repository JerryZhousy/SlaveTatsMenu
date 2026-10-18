#include "core/Config.h"
#include "core/Translation.h"

#include "SimpleIni.h"

namespace stm
{
    Config& Config::Get()
    {
        static Config s_config;
        return s_config;
    }

    std::filesystem::path Config::IniPath()
    {
        static const std::filesystem::path s_path =
            std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SlaveTatsMenu" / "SlaveTatsMenu.ini";
        return s_path;
    }

    void Config::Load()
    {
        const auto path = IniPath();
        const auto pathStr = path.string();

        CSimpleIniA ini;
        ini.SetUnicode();

        const SI_Error rc = ini.LoadFile(pathStr.c_str());
        if (rc < 0) {
            SKSE::log::warn("Config: could not read {} (error {}), writing defaults", pathStr, static_cast<int>(rc));
            Save();
            // Deliberately no early return: the reads below fall back to their
            // defaults against the empty INI, and skipping them also skipped
            // Translation::Load() - which left the whole UI showing raw $STM_ keys
            // on a first run where the INI was missing.
        }

        toggleKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "ToggleKey", 0x44));
        defaultAlpha = std::clamp(static_cast<float>(ini.GetDoubleValue("General", "DefaultAlpha", 1.0)), 0.05f, 1.0f);
        defaultColor = static_cast<std::uint32_t>(ini.GetLongValue("General", "DefaultColor", 0x000000)) & 0xFFFFFF;
        language = ini.GetValue("General", "Language", "en");
        Translation::Get().Load(language);

        // The INI is user editable, so every enum and every range gets clamped:
        // an out-of-range value used to produce an invisible window (alpha 0) or
        // an undefined SlotPolicy with no way to recover from inside the UI.
        const long slotPolicyRaw = ini.GetLongValue("General", "SlotPolicy", 0);
        slotPolicy = (slotPolicyRaw == 1) ? SlotPolicy::kOverwrite : SlotPolicy::kAutoFreeSlot;

        windowWidth = std::clamp(static_cast<float>(ini.GetDoubleValue("UI", "WindowWidth", 1280.0)), 640.0f, 3840.0f);
        windowHeight = std::clamp(static_cast<float>(ini.GetDoubleValue("UI", "WindowHeight", 720.0)), 400.0f, 2160.0f);
        windowAlpha = std::clamp(static_cast<float>(ini.GetDoubleValue("UI", "WindowAlpha", 0.95)), 0.20f, 1.0f);
        fontScale = std::clamp(static_cast<float>(ini.GetDoubleValue("UI", "FontScale", 1.0)), 0.75f, 1.5f);
        showPreview = ini.GetBoolValue("UI", "ShowPreview", true);
        previewSize = std::clamp(static_cast<float>(ini.GetDoubleValue("UI", "PreviewSize", 96.0)), 32.0f, 512.0f);
        long mCol = ini.GetLongValue("UI", "MannequinColor", 1);
        if (mCol < 0 || mCol > 1) mCol = 1;
        mannequinColor = static_cast<MannequinColor>(mCol);
        slotRefreshMs = std::clamp(static_cast<int>(ini.GetLongValue("UI", "SlotRefreshMs", 250)), 50, 5000);

        probeEnabled = ini.GetBoolValue("Debug", "Probe", false);
        probeIntervalMs = std::clamp(static_cast<int>(ini.GetLongValue("Debug", "ProbeIntervalMs", 1000)), 100, 60000);
        probeArea = ini.GetValue("Debug", "ProbeArea", "Body");
        probeSlots = std::clamp(static_cast<int>(ini.GetLongValue("Debug", "ProbeSlots", 12)), 1, 24);
        callTraceEnabled = ini.GetBoolValue("Debug", "CallTrace", false);

        SKSE::log::info("Config: loaded {} (ToggleKey=0x{:X}, Lang={}, WinAlpha={:.2f}, FontScale={:.2f}, SlotPolicy={})",
            pathStr,
            toggleKey,
            language,
            windowAlpha,
            fontScale,
            slotPolicy == SlotPolicy::kOverwrite ? "overwrite" : "autofree");
    }

    void Config::Save()
    {
        const auto path = IniPath();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            SKSE::log::error("Config: could not create directory {}: {}", path.parent_path().string(), ec.message());
        }

        const auto pathStr = path.string();

        CSimpleIniA ini;
        ini.SetUnicode();

        // 先尝试加载已有文件，以保留原 INI 文件中的所有注释和文档排版
        ini.LoadFile(pathStr.c_str());

        ini.SetLongValue("General", "ToggleKey", toggleKey, nullptr, /*a_bUseHex=*/true);
        ini.SetDoubleValue("General", "DefaultAlpha", defaultAlpha);
        ini.SetLongValue("General", "DefaultColor", defaultColor, nullptr, /*a_bUseHex=*/true);
        ini.SetValue("General", "Language", language.c_str());
        ini.SetLongValue("General", "SlotPolicy", static_cast<long>(slotPolicy));

        ini.SetDoubleValue("UI", "WindowWidth", windowWidth);
        ini.SetDoubleValue("UI", "WindowHeight", windowHeight);
        ini.SetDoubleValue("UI", "WindowAlpha", windowAlpha);
        ini.SetDoubleValue("UI", "FontScale", fontScale);
        ini.SetBoolValue("UI", "ShowPreview", showPreview);
        ini.SetDoubleValue("UI", "PreviewSize", previewSize);
        ini.SetLongValue("UI", "MannequinColor", static_cast<long>(mannequinColor));
        ini.SetLongValue("UI", "SlotRefreshMs", slotRefreshMs);

        ini.SetBoolValue("Debug", "Probe", probeEnabled);
        ini.SetLongValue("Debug", "ProbeIntervalMs", probeIntervalMs);
        ini.SetValue("Debug", "ProbeArea", probeArea.c_str());
        ini.SetLongValue("Debug", "ProbeSlots", probeSlots);
        ini.SetBoolValue("Debug", "CallTrace", callTraceEnabled);

        const SI_Error rc = ini.SaveFile(pathStr.c_str());
        if (rc < 0) {
            SKSE::log::error("Config: could not write {} (error {})", pathStr, static_cast<int>(rc));
        } else {
            SKSE::log::info("Config: wrote defaults to {}", pathStr);
        }
    }

    bool Config::IsChinese() const
    {
        return Translation::NormalizeLanguage(language) == "CHINESE";
    }
}
