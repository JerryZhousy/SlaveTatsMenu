#include "data/PresetStore.h"

#include "core/Log.h"
#include "core/Text.h"

#include <fstream>
#include <iterator>

namespace stm::data
{
    namespace
    {
        bool ReadWholeFile(const std::filesystem::path& a_path, std::string& a_out)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            a_out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return !a_out.empty();
        }

        bool WriteWholeFile(const std::filesystem::path& a_path, const std::string& a_data, std::string* a_error)
        {
            std::error_code ec;
            std::filesystem::create_directories(a_path.parent_path(), ec);
            if (ec) {
                if (a_error) {
                    *a_error = "create_directories: " + ec.message();
                }
                return false;
            }

            std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                if (a_error) {
                    *a_error = "could not open the file for writing";
                }
                return false;
            }

            file << a_data;
            if (!file.good()) {
                if (a_error) {
                    *a_error = "write failed";
                }
                return false;
            }
            return true;
        }

        // Refresh() lists whatever is on disk, verbatim. Load()/Delete() then
        // used to sanitise the name again, which does not round-trip for every
        // possible file stem: SanitizePresetName replaces characters, strips
        // leading dots and truncates at kMaxNameLength. A preset whose stem is
        // longer than that limit was therefore listed but could never be opened
        // - the lookup pointed at a truncated filename that does not exist.
        //
        // So: use the name as given when it is a plain filename, and only fall
        // back to the sanitised form (which is what Save() wrote).
        bool IsPlainFileName(std::string_view a_name)
        {
            if (a_name.empty() || a_name.size() > 255) {
                return false;
            }
            if (a_name == "." || a_name == "..") {
                return false;
            }
            for (const char c : a_name) {
                switch (c) {
                case '/':
                case '\\':
                case ':':
                case '*':
                case '?':
                case '"':
                case '<':
                case '>':
                case '|':
                    return false;
                default:
                    break;
                }
            }
            return true;
        }

        // Resolve a preset name to a file: the verbatim stem when that file
        // exists, otherwise the sanitised form that Save() writes.
        std::filesystem::path ResolvePresetFile(const std::filesystem::path& a_dir, std::string_view a_name)
        {
            if (IsPlainFileName(a_name)) {
                const auto direct = a_dir / (stm::text::Utf8ToWide(a_name) + L".json");
                std::error_code ec;
                if (std::filesystem::is_regular_file(direct, ec) && !ec) {
                    return direct;
                }
            }

            return a_dir / (stm::text::Utf8ToWide(SanitizePresetName(a_name)) + L".json");
        }
    }

    PresetStore& PresetStore::Get()
    {
        static PresetStore s_store;
        return s_store;
    }

    std::filesystem::path PresetStore::Directory() const
    {
        static const std::filesystem::path s_dir =
            std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SlaveTatsMenu" / "Presets";
        return s_dir;
    }

    void PresetStore::Refresh()
    {
        _names.clear();

        std::error_code ec;
        if (!std::filesystem::exists(Directory(), ec)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(Directory(), ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }

            // UTF-8, not the ANSI code page - preset names are usually CJK here.
            _names.push_back(stm::text::WideToUtf8(entry.path().stem().wstring()));
        }

        std::sort(_names.begin(), _names.end());
        SKSE::log::info("PresetStore: {} preset(s) available", _names.size());
    }

    bool PresetStore::Load(std::string_view a_name, Preset& a_out, std::string* a_error) const
    {
        const auto path = ResolvePresetFile(Directory(), a_name);

        std::string content;
        if (!ReadWholeFile(path, content)) {
            if (a_error) {
                *a_error = "could not read " + stm::text::WideToUtf8(path.filename().wstring());
            }
            return false;
        }

        return DeserializePreset(content, a_out, a_error);
    }

    bool PresetStore::Save(const Preset& a_preset, std::string* a_error)
    {
        Preset copy = a_preset;
        copy.name = SanitizePresetName(a_preset.name);

        const auto path = Directory() / (stm::text::Utf8ToWide(copy.name) + L".json");
        if (!WriteWholeFile(path, SerializePreset(copy), a_error)) {
            return false;
        }

        SKSE::log::info("PresetStore: wrote '{}' with {} entries", copy.name, copy.entries.size());
        Refresh();
        return true;
    }

    bool PresetStore::Exists(std::string_view a_name) const
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(ResolvePresetFile(Directory(), a_name), ec) && !ec;
    }

    bool PresetStore::Delete(std::string_view a_name)
    {
        const auto path = ResolvePresetFile(Directory(), a_name);

        std::error_code ec;
        const bool removed = std::filesystem::remove(path, ec);
        if (removed) {
            SKSE::log::info("PresetStore: deleted '{}'", stm::text::WideToUtf8(path.filename().wstring()));
            Refresh();
        } else if (ec) {
            SKSE::log::warn("PresetStore: could not delete '{}': {}", stm::text::WideToUtf8(path.filename().wstring()), ec.message());
        }
        return removed;
    }
}
