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

    enum class BodySex
    {
        kFemale = 0,
        kMale = 1
    };

    class NifMeshExtractor
    {
    public:
        // 解析指定 NIF 文件中的几何网格 (支持 Skyrim SE / AE 的 BSTriShape 与 NiSkinPartition)
        static ExtractedMesh LoadFromNif(const std::string& a_filePath);

        // 尝试自动寻找并加载游戏中当前生效的指定部位 NIF (支持身体、面部、手部、脚部与指定性别)
        static ExtractedMesh LoadDefaultPartNif(PreviewPart a_part, BodySex a_sex = BodySex::kFemale);

        // 兼容原有的身体 NIF 加载快捷接口
        static ExtractedMesh LoadDefaultBodyNif(BodySex a_sex = BodySex::kFemale)
        {
            return LoadDefaultPartNif(PreviewPart::kBody, a_sex);
        }

        // 计算平滑法线
        static void ComputeSmoothNormals(std::vector<MannequinVertex>& a_verts, const std::vector<std::uint32_t>& a_indices);

    private:
        static bool ParseDynamicTriShape(
            const std::uint8_t* a_shapeData,
            std::size_t a_shapeSize,
            const std::uint8_t* a_partitionData,
            std::size_t a_partitionSize,
            std::uint32_t a_bsVersion,
            std::vector<MannequinVertex>& a_outVerts,
            std::vector<std::uint32_t>& a_outIndices
        );

        static bool ParseNiSkinPartition(
            const std::uint8_t* a_data,
            std::size_t a_size,
            std::uint32_t a_bsVersion,
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
