#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace stm::obody
{
    struct PresetSlider
    {
        std::string name;
        // Final morph value for a body at weight 100, which is the mesh this
        // preview always renders. OBody computes ((max - min) * weight) + min;
        // at weight 1 that is just `max`, so only the "big" value survives.
        float value = 0.0f;
    };

    struct BodyPreset
    {
        std::string               name;   // <Preset name="...">, not the file name
        std::string               set;    // <Preset set="...">
        // Sex is only known when the file named a set: a preset carries no other
        // hint, and defaulting to "female" for an unnamed set would reject
        // perfectly good male presets.
        bool                      sexKnown = false;
        bool                      female = true;  // meaningful only when sexKnown
        std::vector<PresetSlider> sliders;
    };

    // OBody reads every XML in Data\CalienteTools\BodySlide\SliderPresets and
    // so do we, so a preset OBody can apply is always a preset we can draw.
    //
    // Returns a shared_ptr on purpose: the database is rebuilt whenever the
    // folder changes, and a raw pointer into the map dangles for any caller that
    // outlives the reload. Holding the pointer keeps the preset alive.
    //
    // No disk I/O once the database is loaded - but the first call loads it,
    // which is expensive. Call this from the worker thread, not the game thread;
    // PresetsLoaded() is the cheap question the game thread is allowed to ask.
    std::shared_ptr<const BodyPreset> FindPreset(std::string_view a_name);

    // True once the database has been scanned at least once. Lets the game
    // thread decide whether it can answer "does this preset exist" without
    // paying for the scan itself.
    bool PresetsLoaded();

    // Rate-limited re-scan: picks up presets dropped in mid-session. Stats every
    // XML in the folder, so it belongs on a worker thread too. Returns true when
    // the database was actually rebuilt.
    bool RefreshPresets();

    std::size_t PresetCount();

    const std::string& PresetDirectory();

    // Drops the cache; the next lookup rescans. Live shared_ptrs stay valid.
    void InvalidatePresets();
}
