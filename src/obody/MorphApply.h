#pragma once

#include "obody/TriFile.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stm::obody
{
    // How the preview mesh's vertex positions relate to the coordinates the
    // tri file was authored in.
    //
    // The NIF parser recentres the mesh, scales it to fit the viewport
    // (1.9 / maxDim) and swaps Y with Z so the model stands up. Morph offsets
    // are in raw NIF units, so they need the same scale and the same swap;
    // applying them unscaled moves a vertex by roughly a body length.
    struct VertexSpace
    {
        float scale = 1.0f;
        bool  yzSwapped = true;

        // Raw NIF coordinates: what the offline test feeds in.
        static VertexSpace NifSpace() { return VertexSpace{ 1.0f, false }; }
    };

    struct AppliedMorphInfo
    {
        std::string shapeName;
        std::size_t morphs = 0;   // sliders that actually had a morph in this shape
        std::size_t offsets = 0;  // vertices touched
    };

    // True when every offset in the shape addresses a vertex that exists in the
    // mesh. A tri from a different body build fails this, which is the cheapest
    // way to refuse to deform a mesh we would otherwise scramble.
    bool ShapeFits(const TriShape& a_shape, std::uint32_t a_vertexCount);

    // positions: a_vertexCount tightly packed xyz floats, modified in place.
    // Values are applied linearly, exactly as RaceMenu scales a morph delta.
    bool ApplyMorphs(const TriShape& a_shape, const std::vector<std::pair<std::string, float>>& a_values,
                     float* a_positions, std::uint32_t a_vertexCount, const VertexSpace& a_space,
                     AppliedMorphInfo& a_info);
}
