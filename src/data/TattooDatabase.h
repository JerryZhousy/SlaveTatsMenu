#pragma once

#include "PCH.h"

#include "data/CacheParser.h"

namespace stm::data
{
    struct SectionInfo
    {
        std::string name;
        std::size_t count = 0;
    };

    // The catalogue of available tattoos.
    //
    // The authoritative list is whatever SlaveTatsNG itself knows about, never
    // our own interpretation of the loose JSON files. Three sources are tried in
    // order of trust:
    //
    //   1. the live C++ API (query_available_tattoos) - what the game will
    //      actually accept right now;
    //   2. Data/slavetats_cache.json - the same cache SlaveTatsNG dumps to disk,
    //      used when the API call is unavailable;
    //   3. a scan of Data/Textures/Actors/Character/slavetats - last resort,
    //      may contain entries the game does not recognise.
    //
    // Everything here runs on the game/UI thread; query results are cached and
    // only recomputed when the filter actually changes.
    class TattooDatabase
    {
    public:
        enum class Source
        {
            kNone,
            kSlaveTatsNgApi,
            kCacheFile,
            kJsonScan,
        };

        static TattooDatabase& Get();

        // Rebuilds the full catalogue.
        //
        // a_allowCompile lets the API source ask SlaveTatsNG to compile its cache
        // when that cache looks unbuilt. Upstream only compiles it once a game is
        // loaded, so at kDataLoaded the list is always empty; the compile walks
        // every pack file and costs a moment, which is why the startup call
        // leaves it off and the "a game is loaded now" call turns it on.
        void Rebuild(bool a_allowCompile = false);

        Source      source() const { return _source; }
        const char* SourceName() const;
        bool        Authoritative() const { return _source == Source::kSlaveTatsNgApi || _source == Source::kCacheFile; }

        std::size_t Size() const { return _all.size(); }

        const std::vector<std::string>& Areas() const;

        // Filtered views, cached until the filter changes. The returned
        // reference stays valid until the next Query with a different filter.
        const std::vector<TattooEntry>& Query(std::string_view a_area, std::string_view a_section, std::string_view a_keyword);
        const std::vector<SectionInfo>& QuerySections(std::string_view a_area, std::string_view a_keyword);

        // Find a specific entry by section and name from the full database
        const TattooEntry* Find(std::string_view a_section, std::string_view a_name) const;

    private:
        TattooDatabase() = default;

        bool BuildFromApi(bool a_allowCompile);
        bool BuildFromCacheFile();
        void BuildFromJsonScan();
        void InvalidateQueries();

        Source                   _source = Source::kNone;
        std::vector<TattooEntry> _all;

        // query cache
        std::string              _qArea, _qSection, _qKeyword;
        std::vector<TattooEntry> _qTattoos;
        bool                     _qDirty = true;

        std::string              _qsArea, _qsKeyword;
        std::vector<SectionInfo> _qSections;
        bool                     _qsDirty = true;
    };
}
