#include "core/Config.h"

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
            return;
        }

        toggleKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "ToggleKey", 0x44));
        defaultAlpha = static_cast<float>(ini.GetDoubleValue("General", "DefaultAlpha", 1.0));
        defaultColor = static_cast<std::uint32_t>(ini.GetLongValue("General", "DefaultColor", 0x000000));
        language = std::string_view{ ini.GetValue("General", "Language", "en") } == "zh" ? Language::kChinese : Language::kEnglish;

        windowWidth = static_cast<float>(ini.GetDoubleValue("UI", "WindowWidth", 960.0));
        windowHeight = static_cast<float>(ini.GetDoubleValue("UI", "WindowHeight", 650.0));
        slotRefreshMs = static_cast<int>(ini.GetLongValue("UI", "SlotRefreshMs", 250));

        probeEnabled = ini.GetBoolValue("Debug", "Probe", false);
        probeIntervalMs = static_cast<int>(ini.GetLongValue("Debug", "ProbeIntervalMs", 1000));
        probeArea = ini.GetValue("Debug", "ProbeArea", "Body");
        probeSlots = static_cast<int>(ini.GetLongValue("Debug", "ProbeSlots", 12));
        callTraceEnabled = ini.GetBoolValue("Debug", "CallTrace", true);

        SKSE::log::info("Config: loaded {} (ToggleKey=0x{:X}, Lang={}, SlotRefresh={}ms, Probe={}, CallTrace={})",
            pathStr,
            toggleKey,
            language == Language::kChinese ? "zh" : "en",
            slotRefreshMs,
            probeEnabled,
            callTraceEnabled);
    }

    void Config::Save()
    {
        const auto path = IniPath();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            SKSE::log::error("Config: could not create directory {}: {}", path.parent_path().string(), ec.message());
        }

        CSimpleIniA ini;
        ini.SetUnicode();

        ini.SetLongValue("General", "ToggleKey", toggleKey);
        ini.SetDoubleValue("General", "DefaultAlpha", defaultAlpha);
        ini.SetLongValue("General", "DefaultColor", defaultColor);
        ini.SetValue("General", "Language", language == Language::kChinese ? "zh" : "en");

        ini.SetDoubleValue("UI", "WindowWidth", windowWidth);
        ini.SetDoubleValue("UI", "WindowHeight", windowHeight);
        ini.SetLongValue("UI", "SlotRefreshMs", slotRefreshMs);

        ini.SetBoolValue("Debug", "Probe", probeEnabled);
        ini.SetLongValue("Debug", "ProbeIntervalMs", probeIntervalMs);
        ini.SetValue("Debug", "ProbeArea", probeArea.c_str());
        ini.SetLongValue("Debug", "ProbeSlots", probeSlots);
        ini.SetBoolValue("Debug", "CallTrace", callTraceEnabled);

        const auto pathStr = path.string();
        const SI_Error rc = ini.SaveFile(pathStr.c_str());
        if (rc < 0) {
            SKSE::log::error("Config: could not write {} (error {})", pathStr, static_cast<int>(rc));
        } else {
            SKSE::log::info("Config: wrote defaults to {}", pathStr);
        }
    }
}
