#pragma once

#include <d3d11.h>
#include <string>
#include <string_view>

namespace stm::render
{
    class TextureLoader
    {
    public:
        // Converts arbitrary mod texture paths (e.g. "LewdMarks-glow/001.dds",
        // "Textures/Actors/Character/slavetats/...", or "Data/Textures/...")
        // into the canonical Skyrim VFS path ("textures\actors\character\slavetats\...").
        static std::string NormalizeVfsPath(std::string_view a_rawPath);

        // Loads a DDS texture from Skyrim's Virtual File System (works for both
        // loose files in Data/ and files packed inside .bsa archives).
        // Textures are cached in an LRU cache (default max 32 items).
        // Returns the borrowed ID3D11ShaderResourceView* or nullptr if not found/invalid.
        static ID3D11ShaderResourceView* LoadTattooTexture(const std::string& a_rawPath);

        // Releases all cached textures and frees DirectX resources.
        static void ClearCache();
    };
}
