// Offline self-test for the tattoo data layer.
//
// Exercises CacheParser against a real SlaveTatsNG cache dump and a synthetic
// pack file, so the parsing / normalisation / filtering logic can be verified
// without launching the game. Involves no SKSE, no engine, no game.
//
// Usage: stm_selftest [path-to-slavetats_cache.json]

#include "data/CacheParser.h"
#include "data/PresetIO.h"
#include "diag/SegmentDiff.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool a_condition, const std::string& a_what, const std::string& a_detail = {})
    {
        std::cout << (a_condition ? "  [ ok ] " : "  [FAIL] ") << a_what;
        if (!a_detail.empty()) {
            std::cout << "  (" << a_detail << ")";
        }
        std::cout << "\n";
        if (!a_condition) {
            ++g_failures;
        }
    }

    std::string ReadFile(const std::string& a_path, bool& a_ok)
    {
        std::ifstream file(a_path, std::ios::binary);
        if (!file.is_open()) {
            a_ok = false;
            return {};
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        a_ok = true;
        return buffer.str();
    }
}

int main(int argc, char** argv)
{
    const std::string cachePath = argc > 1 ? argv[1] : "E:/MO_Skyrim/overwrite/slavetats_cache.json";

    std::cout << "=== area normalisation ===\n";
    Check(stm::data::NormalizeArea("BODY") == "Body", "BODY -> Body");
    Check(stm::data::NormalizeArea("Body") == "Body", "Body -> Body");
    Check(stm::data::NormalizeArea("face") == "Face", "face -> Face");
    Check(stm::data::NormalizeArea("hand") == "Hands", "hand -> Hands");
    Check(stm::data::NormalizeArea("HANDS") == "Hands", "HANDS -> Hands");
    Check(stm::data::NormalizeArea("foot") == "Feet", "foot -> Feet");
    Check(stm::data::NormalizeArea("Feet") == "Feet", "Feet -> Feet");
    Check(stm::data::NormalizeArea("torso") == "torso", "unknown area passes through unchanged");
    Check(stm::data::IsKnownArea("Body") && !stm::data::IsKnownArea("torso"), "IsKnownArea");
    Check(stm::data::KnownAreas().size() == 4, "four canonical areas");

    std::cout << "\n=== loose pack file parsing ===\n";
    const std::string packJson = R"([
        {"name":"A","section":"S1","texture":"a\\b.dds","area":"Body"},
        {"name":"B","section":"S2","texture":"c.dds","area":"face","color":16711680,"alpha":0.5},
        {"name":"","section":"S3","texture":"d.dds","area":"Body"},
        {"name":"D","section":"S4","texture":"e.dds","area":"torso"}
    ])";
    {
        std::string       error;
        const auto        entries = stm::data::ParseTattooListJson(packJson, &error);
        Check(error.empty(), "parsed without error", error);
        Check(entries.size() == 2, "only usable entries survive", std::to_string(entries.size()));
        if (entries.size() == 2) {
            Check(entries[0].name == "A" && entries[0].area == "Body", "first entry kept");
            Check(entries[0].defaultAlpha == 1.0f, "alpha defaults to 1");
            Check(entries[1].area == "Face", "map key casing normalised per entry");
            Check(entries[1].defaultColor == 0xFF0000u, "colour read from the pack field", std::to_string(entries[1].defaultColor));
            Check(entries[1].defaultAlpha > 0.49f && entries[1].defaultAlpha < 0.51f, "alpha read from the pack field");
        }
    }

    std::cout << "\n=== real cache dump (" << cachePath << ") ===\n";
    bool       ok = false;
    const auto json = ReadFile(cachePath, ok);
    if (!ok) {
        std::cout << "  [SKIP] could not read the cache file - nothing to compare against\n";
        std::cout << "\n" << (g_failures == 0 ? "ALL AVAILABLE CHECKS PASSED" : std::to_string(g_failures) + " CHECK(S) FAILED") << "\n";
        return g_failures == 0 ? 0 : 1;
    }

    std::string error;
    auto        all = stm::data::ParseCacheJson(json, &error);
    Check(error.empty(), "cache parsed without error", error);
    // Deliberately not a hard-coded number: SlaveTatsNG rewrites this cache every
    // time a game is loaded (the same install has produced 1565 and 1542 entries),
    // so these checks assert structure and internal consistency instead.
    Check(all.size() > 1000, "a plausible number of entries", std::to_string(all.size()));

    std::map<std::string, std::size_t> byArea;
    bool                               allAreasCanonical = true;
    bool                               allHaveTexture = true;
    for (const auto& entry : all) {
        byArea[entry.area]++;
        if (!stm::data::IsKnownArea(entry.area)) {
            allAreasCanonical = false;
        }
        if (entry.texture.empty()) {
            allHaveTexture = false;
        }
    }
    Check(allAreasCanonical, "every entry landed in a canonical area");
    Check(allHaveTexture, "every entry has a texture path");

    for (const auto& [area, count] : byArea) {
        std::cout << "         " << area << " = " << count << "\n";
    }
    const std::size_t areaSum = byArea["Body"] + byArea["Face"] + byArea["Feet"] + byArea["Hands"];
    Check(areaSum == all.size(), "the four areas account for every entry", std::to_string(areaSum));
    Check(byArea["Body"] > byArea["Face"], "Body is the largest area", std::to_string(byArea["Body"]) + " vs " + std::to_string(byArea["Face"]));
    Check(byArea["Face"] > byArea["Hands"], "Face outnumbers Hands");
    Check(byArea["Feet"] > byArea["Hands"], "Feet outnumbers Hands");

    std::cout << "\n=== filtering ===\n";
    Check(stm::data::FilterTattoos(all, { "Face", "", "" }).size() == byArea["Face"], "area filter matches the per-area tally");
    Check(!stm::data::FilterTattoos(all, { "All", "", "" }).empty(), "All is treated as no filter");
    if (!all.empty()) {
        const std::string sampleSection = all.front().section;
        const std::size_t expectedSectionCount = std::count_if(all.begin(), all.end(), [&](const auto& t) {
            return t.section == sampleSection;
        });
        Check(stm::data::FilterTattoos(all, { "", sampleSection, "" }).size() == expectedSectionCount, "section filter matches sample section tally");
    } else {
        Check(stm::data::FilterTattoos(all, { "", "dummy", "" }).empty(), "section filter on empty collection returns empty");
    }
    Check(stm::data::FilterTattoos(all, { "", "", "rbq" }).size() > 0, "keyword is case insensitive");
    Check(stm::data::FilterTattoos(all, { "", "", "zzzzz-nope" }).empty(), "keyword miss returns nothing");
    Check(stm::data::FilterTattoos(all, { "Face", "", "rbq" }).size() <= stm::data::FilterTattoos(all, { "", "", "rbq" }).size(), "filters combine");

    std::cout << "\n=== probe segment diff ===\n";
    Check(stm::diag::DiffSegments("0=0|1=0", "0=0|1=0").empty(), "identical samples yield nothing");
    Check(stm::diag::DiffSegments("0=0|1=0", "0=1|1=0") == "0=1", "one changed segment", stm::diag::DiffSegments("0=0|1=0", "0=1|1=0"));
    Check(stm::diag::DiffSegments("0=0|1=0", "0=1|1=1") == "0=1  1=1", "two changes, two-space separated", stm::diag::DiffSegments("0=0|1=0", "0=1|1=1"));
    Check(stm::diag::DiffSegments("", "0=1|1=1") == "0=1  1=1", "first sample reports everything");
    Check(stm::diag::DiffSegments("0=0|1=0|2=0", "0=0") == "1=<none>  2=<none>", "shorter new sample reports removed segments", stm::diag::DiffSegments("0=0|1=0|2=0", "0=0"));
    Check(stm::diag::DiffSegments("0=0", "0=0|1=1") == "1=1", "appended segment is reported");

    std::cout << "\n=== sorting ===\n";
    auto sorted = all;
    stm::data::SortTattoos(sorted);
    bool ordered = true;
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const auto& prev = sorted[i - 1];
        const auto& curr = sorted[i];
        if (prev.area > curr.area) {
            ordered = false;
            break;
        }
        if (prev.area == curr.area && prev.section > curr.section) {
            ordered = false;
            break;
        }
        if (prev.area == curr.area && prev.section == curr.section && prev.name > curr.name) {
            ordered = false;
            break;
        }
    }
    Check(ordered, "sorted by area, then section, then name");
    Check(sorted.size() == all.size(), "sorting does not lose entries");

    std::cout << "\n=== preset name sanitising ===\n";
    Check(stm::data::SanitizePresetName("My Set 01") == "My Set 01", "plain name survives");
    Check(stm::data::SanitizePresetName("a/b\\c:d") == "a_b_c_d", "path characters replaced", stm::data::SanitizePresetName("a/b\\c:d"));
    Check(stm::data::SanitizePresetName("../../etc") == "______etc", "path traversal neutralised", stm::data::SanitizePresetName("../../etc"));
    Check(stm::data::SanitizePresetName("../../etc").find('/') == std::string::npos && stm::data::SanitizePresetName("a\\b").find('\\') == std::string::npos, "no path separator survives");
    Check(stm::data::SanitizePresetName("") == "preset", "empty falls back to a default");
    Check(stm::data::SanitizePresetName("   ") == "preset", "whitespace only falls back");
    Check(stm::data::SanitizePresetName("灰风的纹身") == "灰风的纹身", "CJK survives intact", stm::data::SanitizePresetName("灰风的纹身"));
    Check(stm::data::SanitizePresetName(std::string(80, 'x')).size() == 48, "length is capped");
    Check(stm::data::SanitizePresetName("CON") == "_CON", "Windows reserved name CON prefixed with underscore");
    Check(stm::data::SanitizePresetName("nul") == "_nul", "Windows reserved name nul case-insensitively prefixed");
    Check(stm::data::SanitizePresetName("COM1") == "_COM1", "Windows reserved name COM1 prefixed");

    std::cout << "\n=== preset round trip ===\n";
    {
        stm::data::Preset preset;
        preset.name = "灰风的纹身";
        preset.sourceActor = "灰风";
        preset.savedAt = "2026-10-09 22:50:00";
        preset.entries.push_back(stm::data::PresetEntry{ "Body", 0, "RBQ_Tattoos", "RBQ 1", 0xFF0000u, 0.7f });
        preset.entries.push_back(stm::data::PresetEntry{ "face", 2, "Makeup", "唇彩", 0x000000u, 1.0f });

        const auto json = stm::data::SerializePreset(preset);
        Check(json.find("灰风") != std::string::npos, "CJK is written unescaped");

        stm::data::Preset loaded;
        std::string      error;
        Check(stm::data::DeserializePreset(json, loaded, &error), "preset parses back", error);
        Check(loaded.entries.size() == 2, "entry count survives", std::to_string(loaded.entries.size()));
        Check(loaded.name == "灰风的纹身", "name survives");
        Check(loaded.sourceActor == "灰风", "source actor survives");
        if (loaded.entries.size() == 2) {
            Check(loaded.entries[0].area == "Body" && loaded.entries[0].slot == 0, "first entry area and slot");
            Check(loaded.entries[0].color == 0xFF0000u, "colour survives");
            Check(loaded.entries[0].alpha > 0.69f && loaded.entries[0].alpha < 0.71f, "alpha survives");
            Check(loaded.entries[1].area == "Face", "lower-case area normalised on load", loaded.entries[1].area);
        }
    }

    std::cout << "\n=== preset rejects garbage ===\n";
    {
        stm::data::Preset out;
        std::string      error;
        Check(!stm::data::DeserializePreset("[]", out, &error), "non-object root rejected", error);
        error.clear();
        Check(!stm::data::DeserializePreset("{\"entries\":[]}", out, &error), "empty entry list rejected", error);
        error.clear();
        Check(!stm::data::DeserializePreset("{\"entries\":[{\"area\":\"torso\",\"name\":\"x\"}]}", out, &error), "unknown area leaves nothing usable", error);
        error.clear();
        Check(!stm::data::DeserializePreset("{not json", out, &error), "malformed json rejected");
    }

    std::cout << "\n" << (g_failures == 0 ? "ALL CHECKS PASSED" : std::to_string(g_failures) + " CHECK(S) FAILED") << "\n";
    return g_failures == 0 ? 0 : 1;
}
