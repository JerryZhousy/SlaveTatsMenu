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

    // ---- colour ------------------------------------------------------------
    //
    // A tattoo colour is a packed 0xAARRGGBB integer, and so is the overlay
    // tint RaceMenu applies it with. The top byte is the tint's STRENGTH, not
    // decoration.
    //
    // 0x00RRGGBB is therefore not "red without an alpha": it is a tint that
    // does nothing. The shader multiplies the diffuse by the tint at strength
    // alpha, so at 0 the texture's own RGB comes through untouched - a black
    // mask renders black, a white mask renders white, and no colour picker has
    // any visible effect. Measured on this install against RaceMenu's own
    // presets, which store the same alpha twice (packed in the tint and as the
    // per-node float beside it).
    //
    // Pack definitions and preset files author the low 24 bits only, so
    // anything read from them has to be lifted before it reaches the game.
    inline constexpr std::uint32_t kOpaqueTint = 0xFF000000u;

    inline constexpr std::uint32_t RgbOf(std::uint32_t a_color)
    {
        return a_color & 0x00FFFFFFu;
    }

    // Full tint strength, colour preserved. Black becomes opaque black, which
    // is exactly "black ink" - the one colour 0x00RRGGBB could never reach.
    inline constexpr std::uint32_t MakeOpaqueTint(std::uint32_t a_rgb)
    {
        return kOpaqueTint | RgbOf(a_rgb);
    }

    // Same, except 0 has to stay 0: in the glow field 0 means "does not glow",
    // and lifting it to opaque black would switch the emissive on for every
    // tattoo that never asked for one.
    inline constexpr std::uint32_t MakeOpaqueGlow(std::uint32_t a_rgb)
    {
        return a_rgb == 0 ? 0u : MakeOpaqueTint(a_rgb);
    }

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

    // Case-insensitive substring test. a_lowerNeedle must already be lowercase;
    // the haystack is compared in place, without building a lowercase copy (see
    // FilterTattoos, which runs this over every entry on each keystroke).
    bool ContainsNoCase(std::string_view a_haystack, std::string_view a_lowerNeedle);

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
