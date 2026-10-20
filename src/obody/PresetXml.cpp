#include "obody/PresetXml.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace stm::obody
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::size_t kMaxPresetFileBytes = 32u * 1024u * 1024u;

        std::string ToLower(std::string_view a_text)
        {
            std::string out(a_text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        bool ContainsAny(std::string_view a_haystack, std::initializer_list<std::string_view> a_needles)
        {
            for (const auto needle : a_needles) {
                if (a_haystack.find(needle) != std::string_view::npos) {
                    return true;
                }
            }
            return false;
        }

        // Attribute lookup that only accepts a whole attribute name, so
        // `name=` does not match inside `filename=` or `setname=`.
        std::string_view GetAttr(std::string_view a_tag, std::string_view a_key)
        {
            std::size_t pos = 0;
            while ((pos = a_tag.find(a_key, pos)) != std::string_view::npos) {
                const bool startsWord = pos == 0 || std::isspace(static_cast<unsigned char>(a_tag[pos - 1])) ||
                                        a_tag[pos - 1] == '<';
                std::size_t cursor = pos + a_key.size();
                while (cursor < a_tag.size() && std::isspace(static_cast<unsigned char>(a_tag[cursor]))) {
                    ++cursor;
                }
                if (startsWord && cursor < a_tag.size() && a_tag[cursor] == '=') {
                    ++cursor;
                    while (cursor < a_tag.size() && std::isspace(static_cast<unsigned char>(a_tag[cursor]))) {
                        ++cursor;
                    }
                    if (cursor >= a_tag.size() || (a_tag[cursor] != '"' && a_tag[cursor] != '\'')) {
                        return {};
                    }
                    const char quote = a_tag[cursor++];
                    const auto end = a_tag.find(quote, cursor);
                    if (end == std::string_view::npos) {
                        return {};
                    }
                    return a_tag.substr(cursor, end - cursor);
                }
                pos += a_key.size();
            }
            return {};
        }

        // BodySlide sliders that OBody flips for UNP-family bodies.
        bool IsDefaultSlider(std::string_view a_name)
        {
            static constexpr std::string_view kNames[] = { "breasts", "breastssmall", "nippledistance", "nipplesize",
                                                           "buttcrack", "butt", "buttsmall", "legs", "arms",
                                                           "shoulderwidth" };
            const auto lower = ToLower(a_name);
            for (const auto name : kNames) {
                if (lower == name) {
                    return true;
                }
            }
            return false;
        }

        struct RawSlider
        {
            float min = 0.0f;
            float max = 0.0f;
        };

        // A preset has a few dozen sliders at most; an unordered_map with a small
        // initial bucket count beats the std::map this used to be, and the order
        // never mattered - the result is stored by name.
        using SliderMap = std::unordered_map<std::string, RawSlider>;

        void MergeSlider(SliderMap& a_set, const std::string& a_name, float a_min, float a_max)
        {
            const auto it = a_set.find(a_name);
            if (it == a_set.end()) {
                a_set.emplace(a_name, RawSlider{ a_min, a_max });
                return;
            }
            // Same rule as OBody: the first non-zero value wins, and the two
            // size entries fill in opposite ends.
            if (it->second.min == 0.0f && a_min != 0.0f) {
                it->second.min = a_min;
            }
            if (it->second.max == 0.0f && a_max != 0.0f) {
                it->second.max = a_max;
            }
        }

        bool ParsePresetBlock(std::string_view a_block, BodyPreset& a_out)
        {
            const auto openEnd = a_block.find('>');
            if (openEnd == std::string_view::npos) {
                return false;
            }
            const auto header = a_block.substr(0, openEnd);

            const auto name = GetAttr(header, "name");
            if (name.empty()) {
                return false;
            }
            a_out.name = std::string(name);
            a_out.set = std::string(GetAttr(header, "set"));

            const auto lowerSet = ToLower(a_out.set);
            // Only a named set tells us the preset's sex. Leaving it unknown is
            // what keeps a male preset from a custom set usable.
            if (!a_out.set.empty()) {
                a_out.sexKnown = true;
                a_out.female = !ContainsAny(lowerSet, { "himbo", "talos", "sam", "sos", "savren" });
            }
            const bool unp = ContainsAny(lowerSet, { "unp", "coco", "bhunp", "uunp" });

            SliderMap raw;
            raw.reserve(48);

            std::size_t pos = openEnd;
            while ((pos = a_block.find("<SetSlider", pos)) != std::string_view::npos) {
                const auto tagEnd = a_block.find('>', pos);
                if (tagEnd == std::string_view::npos) {
                    break;
                }
                const auto tag = a_block.substr(pos, tagEnd - pos);

                const auto sliderName = GetAttr(tag, "name");
                const auto size = GetAttr(tag, "size");
                const auto value = GetAttr(tag, "value");
                if (!sliderName.empty() && !value.empty()) {
                    // from_chars instead of strtof: no NUL-terminated copy per
                    // slider, which is a few thousand allocations per scan. It
                    // does not skip leading blanks the way strtof did, so do it
                    // here - a hand-edited XML is allowed to have them.
                    const char* first = value.data();
                    const char* last = first + value.size();
                    while (first < last && std::isspace(static_cast<unsigned char>(*first))) {
                        ++first;
                    }
                    float parsedValue = 0.0f;
                    if (std::from_chars(first, last, parsedValue).ec != std::errc{}) {
                        pos = tagEnd + 1;
                        continue;
                    }
                    float parsed = parsedValue / 100.0f;
                    if (unp && IsDefaultSlider(sliderName)) {
                        parsed = 1.0f - parsed;
                    }
                    if (size == "big") {
                        MergeSlider(raw, std::string(sliderName), 0.0f, parsed);
                    } else {
                        MergeSlider(raw, std::string(sliderName), parsed, 0.0f);
                    }
                }

                pos = tagEnd + 1;
            }

            a_out.sliders.reserve(raw.size());
            for (const auto& [sliderName, values] : raw) {
                // Weight 100: OBody applies ((max - min) * 1) + min, i.e. max.
                if (values.max == 0.0f) {
                    continue;
                }
                a_out.sliders.push_back(PresetSlider{ sliderName, values.max });
            }

            return !a_out.sliders.empty();
        }

        std::string ReadFile(const fs::path& a_path)
        {
            std::ifstream stream(a_path, std::ios::binary);
            if (!stream.is_open()) {
                return {};
            }
            stream.seekg(0, std::ios::end);
            const auto size = stream.tellg();
            if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxPresetFileBytes) {
                return {};
            }
            stream.seekg(0, std::ios::beg);
            std::string text(static_cast<std::size_t>(size), '\0');
            stream.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (static_cast<std::size_t>(stream.gcount()) != text.size()) {
                return {};
            }
            return text;
        }

        struct Database
        {
            // key: lowercase name. Presets are shared_ptr so a lookup result
            // stays valid across a rebuild of this map.
            std::unordered_map<std::string, std::shared_ptr<const BodyPreset>> byName;
            std::uint64_t                                                      stamp = 0;
        };

        std::mutex        g_mutex;
        Database          g_db;
        bool              g_loaded = false;
        // Mirrors g_loaded for callers that must not wait: the first scan holds
        // g_mutex for the whole folder, and the game thread asking "is it ready
        // yet" must not queue behind that.
        std::atomic<bool> g_loadedFlag{ false };
        std::chrono::steady_clock::time_point g_lastScan{};
    }

    const std::string& PresetDirectory()
    {
        static const std::string s_directory = (fs::current_path() / "Data" / "CalienteTools" / "BodySlide" /
                                                "SliderPresets")
                                                   .string();
        return s_directory;
    }

    void InvalidatePresets()
    {
        std::scoped_lock lock(g_mutex);
        g_loaded = false;
        g_loadedFlag.store(false, std::memory_order_release);
        g_db = {};
    }

    namespace
    {
        std::uint64_t ComputeStamp(const fs::path& a_directory)
        {
            // One error_code per operation: reusing a single one means a failure
            // on the third file aborts the scan and leaves a partial stamp that
            // then never matches again.
            std::error_code ec;
            std::uint64_t   stamp = 0;
            for (const auto& entry : fs::directory_iterator(a_directory, ec)) {
                if (ec) {
                    break;
                }
                std::error_code entryEc;
                if (!entry.is_regular_file(entryEc) || entryEc) {
                    continue;
                }
                if (entry.path().extension() != ".xml") {
                    continue;
                }
                const auto time = entry.last_write_time(entryEc);
                if (entryEc) {
                    continue;
                }
                stamp += static_cast<std::uint64_t>(time.time_since_epoch().count()) & 0xFFFFFFFFull;
                stamp ^= static_cast<std::uint64_t>(entry.path().filename().string().size()) << 17;
            }
            return stamp;
        }

        void LoadDatabaseLocked()
        {
            g_db = {};
            g_loaded = true;
            g_lastScan = std::chrono::steady_clock::now();

            // The flag is published at the END of the scan, never at the start:
            // "loaded" has to mean "the map is complete and safe to read", or a
            // caller would see it set and then block on the mutex it was trying
            // to avoid.
            const fs::path directory(PresetDirectory());
            std::error_code ec;
            if (!fs::is_directory(directory, ec)) {
                g_db.stamp = 0;
                g_loadedFlag.store(true, std::memory_order_release);
                return;
            }

            g_db.stamp = ComputeStamp(directory);
            g_db.byName.reserve(512);

            for (const auto& entry : fs::directory_iterator(directory, ec)) {
                if (ec) {
                    break;
                }
                std::error_code entryEc;
                if (!entry.is_regular_file(entryEc) || entryEc) {
                    continue;
                }
                if (entry.path().extension() != ".xml") {
                    continue;
                }

                const auto text = ReadFile(entry.path());
                if (text.empty()) {
                    continue;
                }

                std::string_view view(text);
                // Skip a UTF-8 BOM; the tag scan does not care about it, but
                // keeping it out makes the first find() cheaper.
                if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF &&
                    static_cast<unsigned char>(view[1]) == 0xBB && static_cast<unsigned char>(view[2]) == 0xBF) {
                    view.remove_prefix(3);
                }

                std::size_t pos = 0;
                while ((pos = view.find("<Preset", pos)) != std::string_view::npos) {
                    const std::size_t after = pos + 7;
                    if (after < view.size()) {
                        const char c = view[after];
                        if (!std::isspace(static_cast<unsigned char>(c)) && c != '>' && c != '/') {
                            pos = after;
                            continue;
                        }
                    }

                    const auto blockEnd = view.find("</Preset>", after);
                    const auto nextOpen = view.find("<Preset", after);
                    std::size_t end = blockEnd;
                    if (nextOpen != std::string_view::npos && (end == std::string_view::npos || nextOpen < end)) {
                        end = nextOpen;
                    }
                    if (end == std::string_view::npos) {
                        end = view.size();
                    }

                    BodyPreset preset;
                    if (ParsePresetBlock(view.substr(pos, end - pos), preset)) {
                        auto key = ToLower(preset.name);
                        // First definition wins, matching OBody's "first file in
                        // alphabetical order" behaviour closely enough.
                        g_db.byName.try_emplace(std::move(key),
                            std::make_shared<const BodyPreset>(std::move(preset)));
                    }

                    pos = end;
                }
            }

            g_loadedFlag.store(true, std::memory_order_release);
        }
    }

    std::shared_ptr<const BodyPreset> FindPreset(std::string_view a_name)
    {
        std::scoped_lock lock(g_mutex);

        if (!g_loaded) {
            LoadDatabaseLocked();
        }

        const auto it = g_db.byName.find(ToLower(a_name));
        return it == g_db.byName.end() ? nullptr : it->second;
    }

    bool PresetsLoaded()
    {
        // No lock: this is the question the game thread asks, and the answer only
        // ever goes false -> true.
        return g_loadedFlag.load(std::memory_order_acquire);
    }

    bool RefreshPresets()
    {
        std::scoped_lock lock(g_mutex);

        if (!g_loaded) {
            LoadDatabaseLocked();
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastScan <= std::chrono::seconds(5)) {
            return false;
        }

        // A miss is the one case worth re-checking the disk for - the user may
        // have dropped a preset in without restarting - but only when the folder
        // actually changed. This stats every XML, so callers run it off the game
        // thread.
        if (ComputeStamp(PresetDirectory()) == g_db.stamp) {
            g_lastScan = now;
            return false;
        }

        LoadDatabaseLocked();
        return true;
    }

    std::size_t PresetCount()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_loaded) {
            LoadDatabaseLocked();
        }
        return g_db.byName.size();
    }
}
