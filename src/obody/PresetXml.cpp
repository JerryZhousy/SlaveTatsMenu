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

        void AppendUtf8(std::string& a_out, unsigned a_code)
        {
            if (a_code < 0x80u) {
                a_out.push_back(static_cast<char>(a_code));
            } else if (a_code < 0x800u) {
                a_out.push_back(static_cast<char>(0xC0u | (a_code >> 6)));
                a_out.push_back(static_cast<char>(0x80u | (a_code & 0x3Fu)));
            } else if (a_code < 0x10000u) {
                a_out.push_back(static_cast<char>(0xE0u | (a_code >> 12)));
                a_out.push_back(static_cast<char>(0x80u | ((a_code >> 6) & 0x3Fu)));
                a_out.push_back(static_cast<char>(0x80u | (a_code & 0x3Fu)));
            } else if (a_code < 0x110000u) {
                a_out.push_back(static_cast<char>(0xF0u | (a_code >> 18)));
                a_out.push_back(static_cast<char>(0x80u | ((a_code >> 12) & 0x3Fu)));
                a_out.push_back(static_cast<char>(0x80u | ((a_code >> 6) & 0x3Fu)));
                a_out.push_back(static_cast<char>(0x80u | (a_code & 0x3Fu)));
            }
        }

        // BodySlide writes preset XML through .NET's XML writer, so anything that
        // is markup-significant in a preset name comes back escaped: a preset
        // called "Lazarus - Safiyya 3BA (NT&AD - Low)" is stored as
        // name="Lazarus - Safiyya 3BA (NT&amp;AD - Low)". OBody parses the file
        // with a real XML reader and therefore knows the decoded name, and that
        // decoded name is what its plugin API hands back. Comparing it against
        // our raw attribute text made every preset with an '&' in its name
        // unresolvable - the preview reported "not found in SliderPresets" for a
        // preset sitting right there on disk.
        std::string DecodeXmlEntities(std::string_view a_text)
        {
            if (a_text.find('&') == std::string_view::npos) {
                return std::string(a_text);
            }

            std::string out;
            out.reserve(a_text.size());

            for (std::size_t i = 0; i < a_text.size(); ++i) {
                if (a_text[i] != '&') {
                    out.push_back(a_text[i]);
                    continue;
                }

                // A bare '&' is not legal XML, but a hand-edited preset may still
                // contain one; copy it through instead of eating the text.
                const auto semicolon = a_text.find(';', i + 1);
                if (semicolon == std::string_view::npos || semicolon - i > 12) {
                    out.push_back('&');
                    continue;
                }

                const auto entity = a_text.substr(i + 1, semicolon - i - 1);
                if (entity == "amp") {
                    out.push_back('&');
                } else if (entity == "lt") {
                    out.push_back('<');
                } else if (entity == "gt") {
                    out.push_back('>');
                } else if (entity == "quot") {
                    out.push_back('"');
                } else if (entity == "apos") {
                    out.push_back('\'');
                } else if (entity.size() > 1 && entity[0] == '#') {
                    const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
                    unsigned   code = 0;
                    bool       valid = entity.size() > (hex ? 2u : 1u);
                    for (std::size_t k = hex ? 2u : 1u; valid && k < entity.size(); ++k) {
                        const char c = entity[k];
                        if (hex && std::isxdigit(static_cast<unsigned char>(c))) {
                            code = code * 16u + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c))
                                                                            ? c - '0'
                                                                            : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
                        } else if (!hex && std::isdigit(static_cast<unsigned char>(c))) {
                            code = code * 10u + static_cast<unsigned>(c - '0');
                        } else {
                            valid = false;
                        }
                        if (code >= 0x110000u) {
                            valid = false;
                        }
                    }
                    if (valid && code != 0) {
                        AppendUtf8(out, code);
                    } else {
                        out.append(a_text.substr(i, semicolon - i + 1));
                    }
                } else {
                    out.append(a_text.substr(i, semicolon - i + 1));
                }

                i = semicolon;
            }

            return out;
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
            a_out.name = DecodeXmlEntities(name);
            a_out.set = DecodeXmlEntities(GetAttr(header, "set"));

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
            // key: lowercase file stem. BodySlide normally keeps the file name
            // and the <Preset name="..."> attribute identical, but a preset that
            // was renamed on disk (or built by a tool that did) leaves OBody
            // holding one and the XML holding the other. Only consulted when the
            // authoritative name lookup misses.
            std::unordered_map<std::string, std::shared_ptr<const BodyPreset>> byFile;
            std::uint64_t                                                      stamp = 0;
        };

        std::mutex        g_mutex;
        Database          g_db;
        bool              g_loaded = false;
        // Mirrors g_loaded for callers that must not wait: the first scan holds
        // g_mutex for the whole folder, and the game thread asking "is it ready
        // yet" must not queue behind that.
        std::atomic<bool> g_loadedFlag{ false };
        // True while a scan holds g_mutex. g_loadedFlag alone is not enough: it
        // stays true during a re-scan, so a game-thread caller would see "ready"
        // and then block in FindPreset() for the entire folder. PresetsLoaded()
        // consults both.
        std::atomic<bool> g_scanning{ false };
        std::chrono::steady_clock::time_point g_lastScan{};

        // Sets g_scanning for the duration of a scan, before the mutex is taken.
        struct ScanScope
        {
            ScanScope() { g_scanning.store(true, std::memory_order_release); }
            ~ScanScope() { g_scanning.store(false, std::memory_order_release); }
            ScanScope(const ScanScope&) = delete;
            ScanScope& operator=(const ScanScope&) = delete;
        };
    }

    const std::string& PresetDirectory()
    {
        // current_path() throws when it is unavailable, and this sits in a
        // function-local static initialiser reached from a render callback: an
        // exception there is std::terminate. Query it with an error_code.
        static const std::string s_directory = [] {
            std::error_code ec;
            auto            root = fs::current_path(ec);
            if (ec) {
                root = fs::path(".");
            }
            return (root / "Data" / "CalienteTools" / "BodySlide" / "SliderPresets").string();
        }();
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
                        auto stored = std::make_shared<const BodyPreset>(std::move(preset));
                        // First definition wins, matching OBody's "first file in
                        // alphabetical order" behaviour closely enough.
                        g_db.byFile.try_emplace(ToLower(entry.path().stem().string()), stored);
                        g_db.byName.try_emplace(std::move(key), std::move(stored));
                    }

                    pos = end;
                }
            }

            g_loadedFlag.store(true, std::memory_order_release);
        }

        // Caller holds g_mutex and the database is loaded. The <Preset name>
        // attribute is what OBody itself matches on, so it wins; the file stem
        // is only a second chance for a preset that was renamed on disk.
        std::shared_ptr<const BodyPreset> LookupLocked(std::string_view a_name)
        {
            const auto key = ToLower(a_name);

            const auto byName = g_db.byName.find(key);
            if (byName != g_db.byName.end()) {
                return byName->second;
            }

            const auto byFile = g_db.byFile.find(key);
            return byFile == g_db.byFile.end() ? nullptr : byFile->second;
        }
    }

    std::shared_ptr<const BodyPreset> FindPreset(std::string_view a_name)
    {
        std::scoped_lock lock(g_mutex);

        if (!g_loaded) {
            LoadDatabaseLocked();
        }

        return LookupLocked(a_name);
    }

    std::shared_ptr<const BodyPreset> TryFindPreset(std::string_view a_name)
    {
        // Never triggers a scan and never waits for one: this runs on the game
        // thread, where a few hundred milliseconds behind g_mutex is a freeze.
        std::unique_lock lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !g_loaded) {
            return nullptr;
        }

        return LookupLocked(a_name);
    }

    bool PresetsLoaded()
    {
        // No lock: this is the question the game thread asks, and it must not
        // queue behind the scanner. It answers false while a scan is in progress
        // as well as before the first one, so "ready" can never be followed by a
        // call that blocks on g_mutex for the whole folder.
        return g_loadedFlag.load(std::memory_order_acquire) && !g_scanning.load(std::memory_order_acquire);
    }

    bool RefreshPresets()
    {
        // Set before the lock: this is a re-scan, so g_loadedFlag is still true
        // and the game thread would otherwise think the database is ready and
        // then block on g_mutex for the whole folder.
        ScanScope scanning;

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

    std::size_t TryPresetCount()
    {
        std::unique_lock lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !g_loaded) {
            return 0;
        }
        return g_db.byName.size();
    }
}
