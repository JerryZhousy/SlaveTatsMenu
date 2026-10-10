#include "data/PresetIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace stm::data
{
    namespace
    {
        constexpr std::size_t kMaxNameLength = 48;

        bool IsSafeAscii(unsigned char a_c)
        {
            return std::isalnum(a_c) != 0 || a_c == ' ' || a_c == '-' || a_c == '_';
        }
    }

    std::string SanitizePresetName(std::string_view a_name)
    {
        std::string out;
        out.reserve(a_name.size());

        for (const unsigned char c : a_name) {
            if (c >= 0x80) {
                // Part of a UTF-8 sequence (CJK names are the common case).
                out.push_back(static_cast<char>(c));
            } else if (IsSafeAscii(c)) {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('_');
            }
        }

        // No leading dots (hidden files, "..") and no trailing spaces.
        while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
            out.erase(out.begin());
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }

        if (out.size() > kMaxNameLength) {
            std::size_t cut = kMaxNameLength;
            // Do not cut in the middle of a UTF-8 sequence.
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            out.resize(cut);
        }

        if (out.empty()) {
            out = "preset";
        }
        return out;
    }

    std::string SerializePreset(const Preset& a_preset)
    {
        nlohmann::json root;
        root["version"] = a_preset.version;
        root["name"] = a_preset.name;
        root["sourceActor"] = a_preset.sourceActor;
        root["savedAt"] = a_preset.savedAt;

        auto entries = nlohmann::json::array();
        for (const auto& entry : a_preset.entries) {
            entries.push_back({
                { "area", entry.area },
                { "slot", entry.slot },
                { "section", entry.section },
                { "name", entry.name },
                { "color", entry.color },
                { "alpha", entry.alpha },
            });
        }
        root["entries"] = std::move(entries);

        // ensure_ascii = false keeps CJK readable in the stored file.
        return root.dump(2, ' ', false);
    }

    bool DeserializePreset(std::string_view a_json, Preset& a_out, std::string* a_error)
    {
        try {
            const auto root = nlohmann::json::parse(a_json.begin(), a_json.end());
            if (!root.is_object()) {
                if (a_error) {
                    *a_error = "preset root is not a JSON object";
                }
                return false;
            }

            const auto entriesIt = root.find("entries");
            if (entriesIt == root.end() || !entriesIt->is_array()) {
                if (a_error) {
                    *a_error = "'entries' is missing or not an array";
                }
                return false;
            }

            Preset preset;
            preset.version = root.value("version", 1);
            preset.name = root.value("name", "");
            preset.sourceActor = root.value("sourceActor", "");
            preset.savedAt = root.value("savedAt", "");

            for (const auto& item : *entriesIt) {
                if (!item.is_object()) {
                    continue;
                }

                PresetEntry entry;
                entry.area = NormalizeArea(item.value("area", ""));
                entry.slot = item.value("slot", -1);
                entry.section = item.value("section", "");
                entry.name = item.value("name", "");
                entry.color = item.value("color", 0x000000);
                entry.alpha = item.value("alpha", 1.0f);

                if (entry.name.empty() || !IsKnownArea(entry.area)) {
                    continue;
                }

                preset.entries.push_back(std::move(entry));
            }

            if (preset.entries.empty()) {
                if (a_error) {
                    *a_error = "preset contains no usable entries";
                }
                return false;
            }

            a_out = std::move(preset);
            return true;
        } catch (const std::exception& e) {
            if (a_error) {
                *a_error = e.what();
            }
            return false;
        }
    }
}
