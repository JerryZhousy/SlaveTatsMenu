#include "data/TattooDatabase.h"

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Log.h"

#include <fstream>
#include <iterator>

namespace stm::data
{
    namespace
    {
        constexpr std::string_view kPool = "SlaveTatsMenu-DB";

        bool ReadWholeFile(const std::filesystem::path& a_path, std::string& a_out)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            a_out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return !a_out.empty();
        }
    }

    TattooDatabase& TattooDatabase::Get()
    {
        static TattooDatabase s_db;
        return s_db;
    }

    const std::vector<std::string>& TattooDatabase::Areas() const
    {
        return KnownAreas();
    }

    const char* TattooDatabase::SourceName() const
    {
        switch (_source) {
        case Source::kSlaveTatsNgApi:
            return "SlaveTatsNG API";
        case Source::kCacheFile:
            return "slavetats_cache.json";
        case Source::kJsonScan:
            return "JSON scan (fallback)";
        default:
            return "none";
        }
    }

    void TattooDatabase::Rebuild(bool a_allowCompile)
    {
        _all.clear();
        _source = Source::kNone;

        if (ng::Ready() && jc::Ready() && BuildFromApi(a_allowCompile)) {
            _source = Source::kSlaveTatsNgApi;
        } else if (BuildFromCacheFile()) {
            _source = Source::kCacheFile;
        } else {
            SKSE::log::warn("TattooDatabase: no usable SlaveTatsNG cache, falling back to a JSON scan - entries may not match what the game can apply");
            BuildFromJsonScan();
            _source = _all.empty() ? Source::kNone : Source::kJsonScan;
        }

        SortTattoos(_all);
        InvalidateQueries();

        SKSE::log::info("TattooDatabase: {} tattoos indexed across {} areas (source: {})", _all.size(), KnownAreas().size(), SourceName());
    }

    bool TattooDatabase::BuildFromApi(bool a_allowCompile)
    {
        auto matches = jc::AddToPool(jc::CreateArray(), kPool);
        if (!matches) {
            SKSE::log::error("TattooDatabase: could not allocate the match array");
            return false;
        }

        // An empty template (id 0) means "no filter" and returns every tattoo,
        // exactly like the MCM does it.
        auto status = ng::QueryAvailable(0, matches.id);

        // Upstream compiles its tattoo cache when a game loads, so the attempt at
        // kDataLoaded queries an empty registry - it even reports success while
        // handing back nothing. Once the player is in game we can ask for the
        // compile explicitly and try again, instead of quietly living on the JSON
        // dump for the rest of the session.
        const bool cacheLooksUnbuilt = status != ng::Status::kOk || jc::ArrayCount(matches) == 0;
        if (a_allowCompile && cacheLooksUnbuilt) {
            SKSE::log::info("TattooDatabase: cache looks unbuilt (status={}, entries={}), asking SlaveTatsNG to compile it",
                ng::ToString(status),
                jc::ArrayCount(matches));

            const int compiled = ng::CompileCache();
            SKSE::log::info("TattooDatabase: compile_cache returned {}", compiled);

            // Start from a clean array: the previous one may hold stale entries.
            jc::CleanPool(kPool);
            matches = jc::AddToPool(jc::CreateArray(), kPool);

            if (matches) {
                status = ng::QueryAvailable(0, matches.id);
            }
        }

        if (status != ng::Status::kOk) {
            SKSE::log::warn("TattooDatabase: query_available_tattoos returned {}, trying the cache file", ng::ToString(status));
            jc::CleanPool(kPool);
            return false;
        }

        const auto count = jc::ArrayCount(matches);
        _all.reserve(static_cast<std::size_t>(std::max(0, count)));

        for (std::int32_t i = 0; i < count; ++i) {
            const auto handle = jc::ArrayGetObj(matches, i);
            if (!handle) {
                continue;
            }

            TattooEntry entry;
            entry.name = jc::MapGetStr(handle, "name");
            entry.section = jc::MapGetStr(handle, "section");
            entry.area = NormalizeArea(jc::MapGetStr(handle, "area"));
            entry.texture = jc::MapGetStr(handle, "texture");
            entry.defaultColor = static_cast<std::uint32_t>(jc::MapGetInt(handle, "color", 0));
            entry.defaultAlpha = 1.0f - jc::MapGetFlt(handle, "invertedAlpha", 0.0f);

            if (entry.name.empty() || !IsKnownArea(entry.area)) {
                continue;
            }
            if (entry.section.empty()) {
                entry.section = "General";
            }

            _all.push_back(std::move(entry));
        }

        jc::CleanPool(kPool);

        if (_all.empty()) {
            SKSE::log::warn("TattooDatabase: the API returned no usable entries, trying the cache file");
            return false;
        }
        return true;
    }

    bool TattooDatabase::BuildFromCacheFile()
    {
        namespace fs = std::filesystem;

        // SlaveTatsNG writes this with JValue::writeToFile(..., "Data/slavetats_cache.json"),
        // which under MO2 lands in the overwrite directory and is virtualised back here.
        const fs::path path = "Data/slavetats_cache.json";

        std::error_code ec;
        if (!fs::exists(path, ec)) {
            SKSE::log::warn("TattooDatabase: {} does not exist", path.string());
            return false;
        }

        std::string content;
        if (!ReadWholeFile(path, content)) {
            SKSE::log::warn("TattooDatabase: {} could not be read", path.string());
            return false;
        }

        std::string error;
        _all = ParseCacheJson(content, &error);

        if (_all.empty()) {
            SKSE::log::warn("TattooDatabase: {} contained no usable entries ({})", path.string(), error.empty() ? "empty" : error);
            return false;
        }
        return true;
    }

    void TattooDatabase::BuildFromJsonScan()
    {
        namespace fs = std::filesystem;

        const fs::path rootDir = L"Data/Textures/Actors/Character/slavetats";
        std::error_code ec;
        if (!fs::exists(rootDir, ec)) {
            SKSE::log::warn("TattooDatabase: {} not found", rootDir.string());
            return;
        }

        std::unordered_set<std::string> seen;

        auto parse = [this, &seen](const fs::path& a_path) {
            const auto key = a_path.string();
            if (!seen.emplace(key).second) {
                return;
            }

            std::string content;
            if (!ReadWholeFile(a_path, content)) {
                return;
            }

            std::string error;
            auto entries = ParseTattooListJson(content, &error);
            if (entries.empty() && !error.empty()) {
                SKSE::log::warn("TattooDatabase: skipping {} ({})", key, error);
                return;
            }

            _all.insert(_all.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
        };

        try {
            for (const auto& entry : fs::directory_iterator(rootDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    parse(entry.path());
                }
            }
        } catch (const std::exception& e) {
            SKSE::log::warn("TattooDatabase: root scan failed ({})", e.what());
        }

        try {
            for (const auto& entry : fs::recursive_directory_iterator(rootDir, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json" && entry.path().parent_path() != rootDir) {
                    parse(entry.path());
                }
            }
        } catch (const std::exception& e) {
            SKSE::log::warn("TattooDatabase: recursive scan failed ({})", e.what());
        }
    }

    void TattooDatabase::InvalidateQueries()
    {
        _qDirty = true;
        _qsDirty = true;
    }

    const std::vector<TattooEntry>& TattooDatabase::Query(std::string_view a_area, std::string_view a_section, std::string_view a_keyword)
    {
        const std::string area(a_area);
        const std::string section(a_section);
        const std::string keyword(a_keyword);

        if (!_qDirty && area == _qArea && section == _qSection && keyword == _qKeyword) {
            return _qTattoos;
        }

        _qArea = area;
        _qSection = section;
        _qKeyword = keyword;
        _qDirty = false;

        const auto matches = FilterTattoos(_all, TattooQuery{ area, section, keyword });

        _qTattoos.clear();
        _qTattoos.reserve(matches.size());
        for (const auto* entry : matches) {
            _qTattoos.push_back(*entry);
        }

        return _qTattoos;
    }

    const std::vector<SectionInfo>& TattooDatabase::QuerySections(std::string_view a_area, std::string_view a_keyword)
    {
        const std::string area(a_area);
        const std::string keyword(a_keyword);

        if (!_qsDirty && area == _qsArea && keyword == _qsKeyword) {
            return _qSections;
        }

        _qsArea = area;
        _qsKeyword = keyword;
        _qsDirty = false;

        _qSections.clear();

        std::unordered_map<std::string, std::size_t> counts;
        for (const auto* entry : FilterTattoos(_all, TattooQuery{ area, {}, {} })) {
            counts[entry->section]++;
        }

        const auto keywordEntry = [&keyword](const std::string& a_section) {
            if (keyword.empty()) {
                return true;
            }
            std::string lower(a_section);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string wanted(keyword);
            std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find(wanted) != std::string::npos;
        };

        _qSections.reserve(counts.size());
        for (auto& [name, count] : counts) {
            if (!keywordEntry(name)) {
                continue;
            }
            _qSections.push_back(SectionInfo{ std::move(name), count });
        }

        std::sort(_qSections.begin(), _qSections.end(), [](const SectionInfo& a, const SectionInfo& b) { return a.name < b.name; });

        return _qSections;
    }
}
