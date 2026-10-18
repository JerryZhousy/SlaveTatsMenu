#pragma once

// Pure data handling for SlaveTats tattoo definitions.
//
// Deliberately free of any SKSE / engine dependency so the parsing, area
// normalisation and filtering can be exercised by an offline self-test against
// real cache files (see tools/selftest). TattooDatabase builds on top of this
// and adds the SKSE-side source selection and caching layer.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stm::data
{
    struct TattooEntry
    {
        std::string   name;
        std::string   section;
        std::string   area;  // canonical spelling: Body / Face / Hands / Feet
        std::string   texture;
        std::uint32_t defaultColor = 0x000000;
        float         defaultAlpha = 1.0f;
        // Emissive colour, the same "glow" integer SlaveTats stores in its
        // ledger: 0xRRGGBB, and black (0) means "does not glow". Packs that
        // never author the field leave it at 0, which is why most of them do
        // not glow at all - the menu lets the user pick one anyway.
        std::uint32_t defaultGlow = 0x000000;
    };

    struct TattooQuery
    {
        std::string area;     // empty or "All" means no area filter
        std::string section;  // empty or "All" means no section filter
        std::string keyword;  // case-insensitive substring match on name or section
    };

    // Canonical area spellings, in display order.
    const std::vector<std::string>& KnownAreas();

    // "BODY" / "body" -> "Body", "face" -> "Face", "hand" -> "Hands".
    // Anything unrecognised is returned unchanged.
    std::string NormalizeArea(std::string_view a_area);

    bool IsKnownArea(std::string_view a_area);

    // Parses Data/slavetats_cache.json as SlaveTatsNG writes it:
    //   domain -> area -> section -> [ {name, section, area, texture, ...} ]
    // The area map keys have inconsistent casing ("BODY", "face", ...), so each
    // entry's own "area" field wins; the map key is only a fallback.
    // Returns an empty vector and fills a_error on failure. Never throws.
    std::vector<TattooEntry> ParseCacheJson(std::string_view a_json, std::string* a_error = nullptr);

    // Parses one loose tattoo pack file: a JSON array of entries. These use
    // "alpha" rather than "invertedAlpha", and default to the Body area.
    std::vector<TattooEntry> ParseTattooListJson(std::string_view a_json, std::string* a_error = nullptr);

    std::vector<const TattooEntry*> FilterTattoos(const std::vector<TattooEntry>& a_all, const TattooQuery& a_query);

    // area, then section, then name - stable.
    void SortTattoos(std::vector<TattooEntry>& a_io);
}
