// Offline check for the OBody add-on's data layer: parses a real .tri and a
// real BodySlide preset XML, applies one to the other, and reports what moved.
//
//   stm_obodytest <body.tri> <preset.xml> [preset name]
//
// No game, no CommonLibSSE: TriFile, PresetXml and MorphApply are pure standard
// library, which is the whole reason they live in their own translation units.

#include "obody/MorphApply.h"
#include "obody/PresetXml.h"
#include "obody/TriFile.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: stm_obodytest <body.tri> <preset.xml> [preset name]\n";
        return 2;
    }

    const std::string triPath = argv[1];
    const std::string xmlPath = argv[2];
    const std::string presetName = (argc >= 4) ? argv[3] : "Vanta Luxe";

    const auto tri = stm::obody::LoadTriFile(triPath);
    if (!tri) {
        std::cerr << "FAIL: could not parse " << triPath << "\n";
        return 1;
    }

    std::cout << "tri: " << tri->shapes.size() << " shapes\n";
    for (const auto& shape : tri->shapes) {
        std::size_t offsets = 0;
        std::uint32_t maxIndex = 0;
        for (const auto& morph : shape.morphs) {
            offsets += morph.offsets.size();
            for (const auto& offset : morph.offsets) {
                maxIndex = (std::max)(maxIndex, static_cast<std::uint32_t>(offset.index));
            }
        }
        std::cout << "  '" << shape.name << "' morphs=" << shape.morphs.size() << " offsets=" << offsets
                  << " maxIndex=" << maxIndex << "\n";
    }

    // The preset database reads from the game's Data tree, so give it one.
    const auto root = fs::temp_directory_path() / "stm_obodytest";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto presets = root / "Data" / "CalienteTools" / "BodySlide" / "SliderPresets";
    fs::create_directories(presets, ec);
    if (ec) {
        std::cerr << "FAIL: could not create " << presets << ": " << ec.message() << "\n";
        return 1;
    }
    fs::copy_file(xmlPath, presets / fs::path(xmlPath).filename(), ec);
    if (ec) {
        std::cerr << "FAIL: could not copy preset: " << ec.message() << "\n";
        return 1;
    }
    fs::current_path(root, ec);

    const auto preset = stm::obody::FindPreset(presetName);
    if (!preset) {
        std::cerr << "FAIL: preset '" << presetName << "' not found (loaded "
                  << stm::obody::PresetCount() << ")\n";
        return 1;
    }

    std::cout << "preset: '" << preset->name << "' set='" << preset->set << "' "
              << (preset->sexKnown ? (preset->female ? "female" : "male") : "sex unset") << " sliders="
              << preset->sliders.size() << "\n";
    for (std::size_t i = 0; i < (std::min)(std::size_t{ 6 }, preset->sliders.size()); ++i) {
        std::cout << "  " << preset->sliders[i].name << " = " << preset->sliders[i].value << "\n";
    }

    // Pick the same shape the renderer would: the one the NIF reported, else
    // the biggest one in the file.
    const auto* shape = tri->FindShape(argc >= 5 ? argv[4] : "Softbody");
    if (!shape) {
        shape = tri->BestShape();
    }
    if (!shape) {
        std::cerr << "FAIL: no usable shape\n";
        return 1;
    }

    std::uint32_t maxIndex = 0;
    for (const auto& morph : shape->morphs) {
        for (const auto& offset : morph.offsets) {
            maxIndex = (std::max)(maxIndex, static_cast<std::uint32_t>(offset.index));
        }
    }
    const std::uint32_t vertexCount = maxIndex + 1;

    std::vector<float> positions(static_cast<std::size_t>(vertexCount) * 3, 0.0f);

    std::vector<std::pair<std::string, float>> values;
    values.reserve(preset->sliders.size());
    for (const auto& slider : preset->sliders) {
        values.emplace_back(slider.name, slider.value);
    }

    stm::obody::AppliedMorphInfo info;
    if (!stm::obody::ApplyMorphs(*shape, values, positions.data(), vertexCount,
            stm::obody::VertexSpace::NifSpace(), info)) {
        std::cerr << "FAIL: no morph from the preset matched shape '" << shape->name << "'\n";
        return 1;
    }

    float maxDelta = 0.0f;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const float* p = positions.data() + i * 3;
        const float  length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (length > 0.0f) {
            ++moved;
        }
        maxDelta = (std::max)(maxDelta, length);
    }

    // The renderer feeds the same offsets through VertexSpace{1.9/maxDim, true};
    // the numbers above are raw NIF units, so a preview-space delta is this
    // times the parser's scale (~0.015 for a 128-unit body).
    std::cout << "applied: shape='" << info.shapeName << "' morphs=" << info.morphs
              << " offsets=" << info.offsets << " vertices=" << moved << "/" << vertexCount
              << " maxDelta=" << maxDelta << "\n";

    // A per-morph peak is the number worth eyeballing: a body slider at 100%
    // moves its own vertices by a fraction of a mesh unit, so anything in the
    // tens means the tri and the preset disagree about scale.
    std::vector<std::pair<float, std::string>> peaks;
    for (const auto& slider : preset->sliders) {
        const auto* morph = shape->FindMorph(slider.name);
        if (!morph) {
            continue;
        }
        float peak = 0.0f;
        for (const auto& offset : morph->offsets) {
            peak = (std::max)(peak, std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z) *
                                        std::abs(slider.value));
        }
        peaks.emplace_back(peak, slider.name);
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < (std::min)(std::size_t{ 3 }, peaks.size()); ++i) {
        std::cout << "  peak " << peaks[i].second << " = " << peaks[i].first << "\n";
    }

    if (maxDelta > 20.0f) {
        std::cerr << "FAIL: displacement looks wrong (" << maxDelta << ")\n";
        return 1;
    }

    std::cout << "OK\n";
    return 0;
}
