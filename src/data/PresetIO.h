#pragma once

// Serialisation for tattoo presets.
//
// A preset is a snapshot of every tattoo applied to one actor: area, slot,
// section, name, colour and opacity. Saving one and applying it to somebody else
// is the whole point of the feature, so the format has to survive hand editing a
// JSON file sitting in the Overwrite folder.
//
// Free of SKSE so the offline self-test can round-trip it.

#include "data/CacheParser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stm::data
{
    struct PresetEntry
    {
        std::string   area;  // canonical: Body / Face / Hands / Feet
        int           slot = -1;
        std::string   section;
        std::string   name;
        std::uint32_t color = 0x000000;
        float         alpha = 1.0f;
    };

    struct Preset
    {
        int                      version = 1;
        std::string              name;
        std::string              sourceActor;  // who it was captured from, informational
        std::string              savedAt;      // local time, informational
        std::vector<PresetEntry> entries;

        bool        Empty() const { return entries.empty(); }
        std::size_t Count() const { return entries.size(); }
    };

    // Turns a user supplied preset name into something safe to use as a file
    // name: letters, digits, space, '-' and '_' survive, multi-byte UTF-8 (CJK)
    // survives, everything else becomes '_'. Never returns an empty string and
    // never produces a leading '.' or a path separator.
    std::string SanitizePresetName(std::string_view a_name);

    std::string SerializePreset(const Preset& a_preset);

    // Fills a_out only on success. Entries with an unknown area or an empty name
    // are dropped rather than failing the whole file.
    bool DeserializePreset(std::string_view a_json, Preset& a_out, std::string* a_error = nullptr);
}
