#include "core/Translation.h"
#include "core/Text.h"

#include <cctype>
#include <fstream>
#include <set>

namespace stm
{
    namespace
    {
        // A language code becomes a file name component. NormalizeLanguage only
        // upper-cases unknown input, so without this an INI value like
        // "..\..\x" would walk out of the translations directory.
        bool IsValidLanguageCode(std::string_view a_code)
        {
            if (a_code.empty() || a_code.size() > 16) {
                return false;
            }
            return std::all_of(a_code.begin(), a_code.end(), [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '_' || c == '-';
            });
        }

        // Interface/Translations/SlaveTatsMenu_template.txt is the reference copy
        // translators start from. Its name matches the SlaveTatsMenu_*.txt glob, so
        // without this it would be offered as a language called "TEMPLATE" and,
        // if picked, would load the untranslated reference over the real strings.
        constexpr std::string_view kTemplateLanguageCode = "TEMPLATE";

        bool IsTemplateLanguage(std::string_view a_normalizedCode)
        {
            return a_normalizedCode == kTemplateLanguageCode;
        }
    }

    Translation& Translation::Get()
    {
        static Translation s_instance;
        return s_instance;
    }

    std::string Translation::NormalizeLanguage(std::string_view a_lang)
    {
        std::string s(a_lang);
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (s == "zh" || s == "chinese" || s == "chs" || s == "cht" || s == "cn") {
            return "CHINESE";
        }
        if (s == "en" || s == "english" || s == "eng" || s == "us" || s == "gb") {
            return "ENGLISH";
        }
        for (auto& c : s) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string Translation::GetLanguageDisplayName(const std::string& a_lang)
    {
        const auto norm = NormalizeLanguage(a_lang);
        if (norm == "CHINESE") return "简体中文 (Chinese)";
        if (norm == "ENGLISH") return "English";
        if (norm == "JAPANESE") return "日本語 (Japanese)";
        if (norm == "RUSSIAN") return "Русский (Russian)";
        if (norm == "FRENCH") return "Français (French)";
        if (norm == "GERMAN") return "Deutsch (German)";
        if (norm == "SPANISH") return "Español (Spanish)";
        return norm;
    }

    std::vector<std::filesystem::path> Translation::GetSearchDirectories()
    {
        std::vector<std::filesystem::path> dirs;

        std::error_code ec;
        auto cur = std::filesystem::current_path(ec);
        if (!ec) {
            dirs.push_back(cur / "Data" / "Interface" / "Translations");
            dirs.push_back(cur / "Interface" / "Translations");
        }

        // Relative to DLL module location: <ModRoot>/SKSE/Plugins/SlaveTatsMenu.dll -> <ModRoot>/Interface/Translations
        HMODULE hModule = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Translation::Get), &hModule)) {
            wchar_t buffer[MAX_PATH];
            if (GetModuleFileNameW(hModule, buffer, MAX_PATH)) {
                std::filesystem::path dllPath(buffer);
                auto modRoot = dllPath.parent_path().parent_path().parent_path();
                dirs.push_back(modRoot / "Interface" / "Translations");
            }
        }

        return dirs;
    }

    std::filesystem::path Translation::FindTranslationFile(std::string_view a_lang)
    {
        const auto        norm = NormalizeLanguage(a_lang);
        if (!IsValidLanguageCode(norm)) {
            SKSE::log::warn("Translation: refusing invalid language code '{}'", std::string(a_lang));
            return {};
        }
        if (IsTemplateLanguage(norm)) {
            SKSE::log::warn("Translation: '{}' is the template, not a language", std::string(a_lang));
            return {};
        }

        const std::string targetFilename = "SlaveTatsMenu_" + norm + ".txt";

        for (const auto& dir : GetSearchDirectories()) {
            std::error_code ec;
            auto candidate = dir / targetFilename;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
        return {};
    }

    std::vector<std::string> Translation::GetAvailableLanguages() const
    {
        // This used to walk up to three directories on every frame the settings
        // tab was open. The set only changes when a language file is installed,
        // so it is cached and invalidated from Load().
        if (_langCacheValid) {
            return _langCache;
        }

        std::set<std::string> langs;
        langs.insert("CHINESE");
        langs.insert("ENGLISH");

        for (const auto& dir : GetSearchDirectories()) {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
                continue;
            }

            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    auto filename = entry.path().filename().string();
                    if (filename.starts_with("SlaveTatsMenu_") && filename.ends_with(".txt")) {
                        std::string langCode = filename.substr(14, filename.size() - 14 - 4);
                        if (!langCode.empty()) {
                            const auto normalized = NormalizeLanguage(langCode);
                            if (IsValidLanguageCode(normalized) && !IsTemplateLanguage(normalized)) {
                                langs.insert(normalized);
                            }
                        }
                    }
                }
            }
        }

        std::vector<std::string> result;
        if (langs.contains("CHINESE")) result.push_back("CHINESE");
        if (langs.contains("ENGLISH")) result.push_back("ENGLISH");
        for (const auto& l : langs) {
            if (l != "CHINESE" && l != "ENGLISH") {
                result.push_back(l);
            }
        }

        _langCache = result;
        _langCacheValid = true;
        return result;
    }

    bool Translation::LoadFileToMap(const std::filesystem::path& a_path, std::unordered_map<std::string, std::string>& a_map)
    {
        std::ifstream file(a_path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            return false;
        }

        std::string textUtf8;
        // Check BOM for UTF-16LE: 0xFF 0xFE
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
            std::size_t wcharCount = (bytes.size() - 2) / 2;
            std::wstring wideStr(wcharCount, L'\0');
            std::memcpy(wideStr.data(), bytes.data() + 2, wcharCount * sizeof(wchar_t));
            textUtf8 = text::WideToUtf8(wideStr);
        }
        // Check BOM for UTF-8: 0xEF 0xBB 0xBF
        else if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            textUtf8.assign(reinterpret_cast<const char*>(bytes.data() + 3), bytes.size() - 3);
        }
        else {
            textUtf8.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        // Parse line by line: $KEY<TAB>VALUE
        std::string_view sv(textUtf8);
        std::size_t lineStart = 0;
        while (lineStart < sv.size()) {
            std::size_t lineEnd = sv.find_first_of("\r\n", lineStart);
            if (lineEnd == std::string_view::npos) {
                lineEnd = sv.size();
            }

            std::string_view line = sv.substr(lineStart, lineEnd - lineStart);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);

            if (!line.empty() && line[0] != ';' && line[0] != '#') {
                std::size_t tabPos = line.find('\t');
                if (tabPos != std::string_view::npos) {
                    std::string_view key = line.substr(0, tabPos);
                    std::string_view val = line.substr(tabPos + 1);

                    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.remove_prefix(1);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
                    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);

                    if (!key.empty()) {
                        std::string processedVal;
                        processedVal.reserve(val.size());
                        for (std::size_t i = 0; i < val.size(); ++i) {
                            if (val[i] == '\\' && i + 1 < val.size()) {
                                if (val[i + 1] == 'n') {
                                    processedVal += '\n';
                                    ++i;
                                } else if (val[i + 1] == 't') {
                                    processedVal += '\t';
                                    ++i;
                                } else if (val[i + 1] == '\\') {
                                    processedVal += '\\';
                                    ++i;
                                } else {
                                    processedVal += val[i];
                                }
                            } else {
                                processedVal += val[i];
                            }
                        }
                        a_map[std::string(key)] = std::move(processedVal);
                    }
                }
            }

            lineStart = lineEnd;
            while (lineStart < sv.size() && (sv[lineStart] == '\r' || sv[lineStart] == '\n')) {
                ++lineStart;
            }
        }

        return !a_map.empty();
    }

    void Translation::Load(std::string_view a_lang)
    {
        _currentLang = NormalizeLanguage(a_lang);
        if (IsTemplateLanguage(_currentLang)) {
            // An INI pointing at the shipped template is a mistake, not a language
            // choice: fall back instead of rendering the untranslated reference copy.
            SKSE::log::warn("Translation: '{}' is the template, not a language; using ENGLISH", _currentLang);
            _currentLang = "ENGLISH";
        }
        _langCacheValid = false;

        // Always load ENGLISH as base fallback if not already loaded
        if (_fallbackDict.empty() || _currentLang == "ENGLISH") {
            _fallbackDict.clear();
            auto enPath = FindTranslationFile("ENGLISH");
            if (!enPath.empty()) {
                LoadFileToMap(enPath, _fallbackDict);
                SKSE::log::info("Translation: loaded {} fallback entries from {}", _fallbackDict.size(), enPath.string());
            } else {
                SKSE::log::warn("Translation: ENGLISH translation file not found in search paths");
            }
        }

        // Load target language dictionary
        _activeDict.clear();
        if (_currentLang == "ENGLISH") {
            _activeDict = _fallbackDict;
        } else {
            auto langPath = FindTranslationFile(_currentLang);
            if (!langPath.empty()) {
                LoadFileToMap(langPath, _activeDict);
                SKSE::log::info("Translation: loaded {} entries for {} from {}", _activeDict.size(), _currentLang, langPath.string());
            } else {
                SKSE::log::warn("Translation: file for {} not found, using ENGLISH fallback", _currentLang);
            }
        }
    }

    const char* Translation::Tr(const char* a_key, const char* a_fallback) const
    {
        if (!a_key || a_key[0] == '\0') {
            return a_fallback ? a_fallback : "";
        }

        auto it = _activeDict.find(a_key);
        if (it != _activeDict.end()) {
            return it->second.c_str();
        }

        it = _fallbackDict.find(a_key);
        if (it != _fallbackDict.end()) {
            return it->second.c_str();
        }

        return a_fallback ? a_fallback : a_key;
    }
}
