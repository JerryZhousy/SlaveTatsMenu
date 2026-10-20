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
    // TryFindPreset() is the variant the game thread is allowed to use.
    std::shared_ptr<const BodyPreset> FindPreset(std::string_view a_name);

    // Non-blocking lookup for the game thread.
    //
    // FindPreset() takes the database mutex, and RefreshPresets() holds that
    // mutex for the whole folder scan (tens of megabytes of XML). A caller that
    // cannot afford to wait must not queue behind it, so this returns nullptr
    // when the lock is busy. The worker resolves the preset again in ApplyImpl,
    // so a miss here only means "not known yet", never "does not exist".
    std::shared_ptr<const BodyPreset> TryFindPreset(std::string_view a_name);

    // True once the database has been scanned at least once. Lets the game
    // thread decide whether it can answer "does this preset exist" without
    // paying for the scan itself. False while a re-scan is in progress, so the
    // answer can never be followed by a blocking FindPreset().
    bool PresetsLoaded();

    // Rate-limited re-scan: picks up presets dropped in mid-session. Stats every
    // XML in the folder, so it belongs on a worker thread too. Returns true when
    // the database was actually rebuilt.
    bool RefreshPresets();

    // Non-blocking preset count, for log lines on the game thread.
    std::size_t TryPresetCount();

    // Blocking variant, for the worker and the offline test.
    std::size_t PresetCount();

    const std::string& PresetDirectory();

    // Drops the cache; the next lookup rescans. Live shared_ptrs stay valid.
    void InvalidatePresets();
}
