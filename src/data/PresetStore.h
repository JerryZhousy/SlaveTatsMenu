#pragma once

#include "PCH.h"

#include "data/PresetIO.h"

namespace stm::data
{
    // Presets live as one JSON file each in
    //   Data/SKSE/Plugins/SlaveTatsMenu/Presets/<name>.json
    // Under MO2 that path resolves through the virtual file system, so writes end
    // up in the overwrite directory and are visible again on the next run.
    class PresetStore
    {
    public:
        static PresetStore& Get();

        std::filesystem::path Directory() const;

        // Re-scans the directory (a handful of files, cheap).
        void Refresh();

        const std::vector<std::string>& Names() const { return _names; }

        bool Load(std::string_view a_name, Preset& a_out, std::string* a_error = nullptr) const;
        bool Save(const Preset& a_preset, std::string* a_error = nullptr);
        bool Delete(std::string_view a_name);

    private:
        PresetStore() = default;

        std::vector<std::string> _names;
    };
}
