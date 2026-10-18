#pragma once

#include "PCH.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stm
{
    class Translation
    {
    public:
        static Translation& Get();

        // Normalizes language identifier to canonical uppercase (e.g. "zh", "chs" -> "CHINESE", "en" -> "ENGLISH")
        static std::string NormalizeLanguage(std::string_view a_lang);

        // Friendly display name for UI dropdown (e.g. "CHINESE" -> "简体中文 (Chinese)")
        static std::string GetLanguageDisplayName(const std::string& a_lang);

        // Scans Data/Interface/Translations for SlaveTatsMenu_*.txt and returns available language names
        std::vector<std::string> GetAvailableLanguages() const;

        // Loads translations for given language. Loads ENGLISH first as fallback, then merges target language.
        void Load(std::string_view a_lang);

        // Looks up a key (e.g. "$STM_Title"). Returns translated string if found, else a_fallback, else a_key.
        const char* Tr(const char* a_key, const char* a_fallback = nullptr) const;

        const std::string& GetCurrentLanguage() const { return _currentLang; }

    private:
        Translation() = default;

        static std::vector<std::filesystem::path> GetSearchDirectories();
        static std::filesystem::path FindTranslationFile(std::string_view a_lang);
        static bool LoadFileToMap(const std::filesystem::path& a_path, std::unordered_map<std::string, std::string>& a_map);

        std::string _currentLang = "ENGLISH";
        std::unordered_map<std::string, std::string> _fallbackDict; // Always English
        std::unordered_map<std::string, std::string> _activeDict;   // Active language

        // GetAvailableLanguages() is called from the settings tab every frame, so
        // its directory scan result is cached and invalidated by Load().
        mutable std::vector<std::string> _langCache;
        mutable bool                     _langCacheValid = false;
    };

    inline const char* Tr(const char* a_key, const char* a_fallback = nullptr)
    {
        return Translation::Get().Tr(a_key, a_fallback);
    }
}
