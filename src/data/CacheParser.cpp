#include "data/CacheParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace stm::data
{
    namespace
    {
        std::string ToLower(std::string_view a_str)
        {
            std::string result(a_str);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        // a_packStyle: loose pack files store "alpha"; the cache stores
        // "invertedAlpha" (and omits it entirely when the pack did not set it).
        TattooEntry FromJsonEntry(const nlohmann::json& a_item, std::string_view a_fallbackSection, std::string_view a_fallbackArea, bool a_packStyle)
        {
            TattooEntry entry;
            entry.name = a_item.value("name", "");
            entry.section = a_item.value("section", std::string(a_fallbackSection));
            entry.area = NormalizeArea(a_item.value("area", std::string(a_fallbackArea)));
            entry.texture = a_item.value("texture", "");
            entry.defaultColor = a_item.value("color", 0x000000);
            // "glow" is a colour, not a scalar: SlaveTats stores the emissive
            // tint in the ledger as 0xRRGGBB, black meaning "no glow".
            entry.defaultGlow = a_item.value("glow", 0x000000);

            if (a_packStyle) {
                entry.defaultAlpha = a_item.value("alpha", 1.0f);
            } else {
                entry.defaultAlpha = 1.0f - a_item.value("invertedAlpha", 0.0f);
            }

            if (entry.section.empty()) {
                entry.section = "General";
            }
            return entry;
        }
    }

    const std::vector<std::string>& KnownAreas()
    {
        static const std::vector<std::string> s_areas{ "Body", "Face", "Hands", "Feet" };
        return s_areas;
    }

    std::string NormalizeArea(std::string_view a_area)
    {
        const auto lower = ToLower(a_area);
        if (lower == "body") {
            return "Body";
        }
        if (lower == "face") {
            return "Face";
        }
        if (lower == "hands" || lower == "hand") {
            return "Hands";
        }
        if (lower == "feet" || lower == "foot") {
            return "Feet";
        }
        return std::string(a_area);
    }

    bool IsKnownArea(std::string_view a_area)
    {
        const auto& areas = KnownAreas();
        return std::find(areas.begin(), areas.end(), a_area) != areas.end();
    }

    std::vector<TattooEntry> ParseCacheJson(std::string_view a_json, std::string* a_error)
    {
        std::vector<TattooEntry> result;

        try {
            const auto root = nlohmann::json::parse(a_json.begin(), a_json.end());
            if (!root.is_object()) {
                if (a_error) {
                    *a_error = "cache root is not a JSON object";
                }
                return result;
            }

            for (auto& [domainName, areas] : root.items()) {
                (void)domainName;
                if (!areas.is_object()) {
                    continue;
                }

                for (auto& [areaKey, sections] : areas.items()) {
                    if (!sections.is_object()) {
                        continue;
                    }

                    for (auto& [sectionKey, items] : sections.items()) {
                        if (!items.is_array()) {
                            continue;
                        }

                        for (const auto& item : items) {
                            if (!item.is_object()) {
                                continue;
                            }

                            auto entry = FromJsonEntry(item, sectionKey, areaKey, false);
                            if (entry.name.empty() || !IsKnownArea(entry.area)) {
                                continue;
                            }

                            result.push_back(std::move(entry));
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            if (a_error) {
                *a_error = e.what();
            }
            result.clear();
        }

        return result;
    }

    std::vector<TattooEntry> ParseTattooListJson(std::string_view a_json, std::string* a_error)
    {
        std::vector<TattooEntry> result;

        try {
            const auto root = nlohmann::json::parse(a_json.begin(), a_json.end());
            if (!root.is_array()) {
                if (a_error) {
                    *a_error = "tattoo pack root is not a JSON array";
                }
                return result;
            }

            for (const auto& item : root) {
                if (!item.is_object()) {
                    continue;
                }

                auto entry = FromJsonEntry(item, "General", "Body", true);
                if (entry.name.empty() || entry.texture.empty() || !IsKnownArea(entry.area)) {
                    continue;
                }

                result.push_back(std::move(entry));
            }
        } catch (const std::exception& e) {
            if (a_error) {
                *a_error = e.what();
            }
            result.clear();
        }

        return result;
    }

    std::vector<const TattooEntry*> FilterTattoos(const std::vector<TattooEntry>& a_all, const TattooQuery& a_query)
    {
        std::vector<const TattooEntry*> result;

        const bool filterArea = !a_query.area.empty() && a_query.area != "All";
        const bool filterSection = !a_query.section.empty() && a_query.section != "All";
        const auto keyword = ToLower(a_query.keyword);

        for (const auto& entry : a_all) {
            if (filterArea && entry.area != a_query.area) {
                continue;
            }
            if (filterSection && entry.section != a_query.section) {
                continue;
            }
            if (!keyword.empty()) {
                if (ToLower(entry.name).find(keyword) == std::string::npos && ToLower(entry.section).find(keyword) == std::string::npos) {
                    continue;
                }
            }
            result.push_back(&entry);
        }

        return result;
    }

    void SortTattoos(std::vector<TattooEntry>& a_io)
    {
        std::stable_sort(a_io.begin(), a_io.end(), [](const TattooEntry& a, const TattooEntry& b) {
            if (a.area != b.area) {
                return a.area < b.area;
            }
            if (a.section != b.section) {
                return a.section < b.section;
            }
            return a.name < b.name;
        });
    }
}
