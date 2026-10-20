#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace stm::obody
{
    // One vertex's share of a morph: `index` is the vertex in the reference
    // mesh, the floats are the displacement at morph value 1.0.
    struct MorphOffset
    {
        std::uint16_t index = 0;
        float         x = 0.0f;
        float         y = 0.0f;
        float         z = 0.0f;
    };

    struct TriMorph
    {
        std::string               name;
        std::string               lowerName;
        std::vector<MorphOffset>  offsets;
    };

    struct TriShape
    {
        std::string             name;
        std::string             lowerName;
        std::vector<TriMorph>   morphs;

        // Case-insensitive: BodySlide and the preset XML do not always agree on
        // capitalisation, and OBody itself matches loosely.
        const TriMorph* FindMorph(std::string_view a_name) const;
    };

    struct TriFile
    {
        std::vector<TriShape> shapes;

        const TriShape* FindShape(std::string_view a_name) const;

        // Fallback when the NIF shape name is not in the tri (a renamed shape,
        // or a body whose tri came from another mod). Picks the shape with the
        // most morph data, which is always the main body in practice.
        const TriShape* BestShape() const;
    };

    // Path + last-write-time cached, thread safe, holds at most two files
    // (female and male) so a preset switch does not re-read 5 MB every time.
    std::shared_ptr<const TriFile> LoadTriFile(const std::string& a_path);
}
