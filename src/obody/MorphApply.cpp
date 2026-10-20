#include "obody/MorphApply.h"

namespace stm::obody
{
    bool ShapeFits(const TriShape& a_shape, std::uint32_t a_vertexCount)
    {
        if (a_vertexCount == 0) {
            return false;
        }

        std::uint32_t maxIndex = 0;
        for (const auto& morph : a_shape.morphs) {
            for (const auto& offset : morph.offsets) {
                if (offset.index >= a_vertexCount) {
                    return false;
                }
                if (offset.index > maxIndex) {
                    maxIndex = offset.index;
                }
            }
        }

        // A body shape's morphs reach nearly every one of its vertices. A shape
        // that only covers a fifth of the mesh is the wrong shape (a garment
        // piece, or a tri from another body build), and deforming with it would
        // scramble the mesh rather than merely look wrong.
        return (static_cast<std::uint64_t>(maxIndex) + 1) * 2 >= a_vertexCount;
    }

    bool ApplyMorphs(const TriShape& a_shape, const std::vector<std::pair<std::string, float>>& a_values,
                     float* a_positions, std::uint32_t a_vertexCount, const VertexSpace& a_space,
                     AppliedMorphInfo& a_info)
    {
        a_info = {};
        a_info.shapeName = a_shape.name;

        if (!a_positions || a_vertexCount == 0) {
            return false;
        }

        for (const auto& [name, value] : a_values) {
            if (value == 0.0f) {
                continue;
            }

            const auto* morph = a_shape.FindMorph(name);
            if (!morph) {
                continue;
            }

            for (const auto& offset : morph->offsets) {
                if (offset.index >= a_vertexCount) {
                    continue;
                }
                // NIF space (X right, Y forward, Z up) -> preview space
                // (X right, Y up, Z forward), then the parser's scale.
                const float dx = offset.x * value * a_space.scale;
                const float dy = (a_space.yzSwapped ? offset.z : offset.y) * value * a_space.scale;
                const float dz = (a_space.yzSwapped ? offset.y : offset.z) * value * a_space.scale;

                float* position = a_positions + static_cast<std::size_t>(offset.index) * 3;
                position[0] += dx;
                position[1] += dy;
                position[2] += dz;
            }

            ++a_info.morphs;
            a_info.offsets += morph->offsets.size();
        }

        return a_info.morphs > 0;
    }
}
