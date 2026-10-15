#pragma once

#include "render/MannequinRenderer.h"
#include <string>
#include <vector>
#include <cstdint>

namespace stm::render
{
    struct ExtractedMesh
    {
        std::vector<MannequinVertex> vertices;
        std::vector<std::uint32_t>   indices;
        std::string                  sourcePath;
        std::string                  shapeName;
        bool                         success = false;
    };

    class NifMeshExtractor
    {
    public:
        // 解析指定 NIF 文件中的身体几何网格 (支持 Skyrim SE / AE 的 BSTriShape 与 NiSkinPartition)
        static ExtractedMesh LoadFromNif(const std::string& a_filePath);

        // 尝试自动寻找并加载游戏中当前生效的女性身体 NIF
        static ExtractedMesh LoadDefaultBodyNif();

    private:
        static bool ParseNiSkinPartition(
            const std::uint8_t* a_data,
            std::size_t a_size,
            std::uint64_t a_vertexDesc,
            std::vector<MannequinVertex>& a_outVerts,
            std::vector<std::uint32_t>& a_outIndices
        );

        static bool ParseDirectTriShape(
            const std::uint8_t* a_data,
            std::size_t a_size,
            std::uint32_t a_numTriangles,
            std::uint32_t a_numVertices,
            std::uint64_t a_vertexDesc,
            std::vector<MannequinVertex>& a_outVerts,
            std::vector<std::uint32_t>& a_outIndices
        );

        // 半精度浮点 (float16) 转单精度浮点 (float32)
        static float HalfToFloat(std::uint16_t a_half);
    };
}
