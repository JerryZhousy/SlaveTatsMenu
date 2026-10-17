#include "render/NifMeshExtractor.h"
#include <SKSE/SKSE.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace stm::render
{
    namespace
    {
        #pragma pack(push, 1)
        struct NifHeaderPrefix
        {
            char     magic[39];
            uint32_t version;
            uint8_t  endian;
            uint32_t userVersion;
            uint32_t numBlocks;
            uint32_t bsVersion;
        };
        #pragma pack(pop)
    }

    void NifMeshExtractor::ComputeSmoothNormals(std::vector<MannequinVertex>& a_verts, const std::vector<std::uint32_t>& a_indices)
    {
        if (a_verts.empty() || a_indices.size() < 3) return;

        // 只能按三角形绕序自己累加, 原因(在真实身体 NIF 上实测):
        //   绕序累加得到的法线: 与"由人体中轴向外"方向平均点积 +0.58, 相邻边夹角中位数 3.7 度
        //   顶点数据 offset 20 的 byte4: 平均点积 -0.02(等于随机), 相邻边夹角中位数 44 度、40% 超 60 度
        // 即那个 byte4 并不是表面法线。曾用它给累加结果定朝向, 结果是逐顶点随机翻符号,
        // 画面上表现为满身深浅斑点 —— 绝不要再这么做。
        for (auto& v : a_verts) {
            v.norm = { 0.0f, 0.0f, 0.0f };
        }

        for (std::size_t i = 0; i + 2 < a_indices.size(); i += 3) {
            auto i0 = a_indices[i];
            auto i1 = a_indices[i + 1];
            auto i2 = a_indices[i + 2];
            if (i0 < a_verts.size() && i1 < a_verts.size() && i2 < a_verts.size()) {
                const auto& p0 = a_verts[i0].pos;
                const auto& p1 = a_verts[i1].pos;
                const auto& p2 = a_verts[i2].pos;

                float e1x = p1.x - p0.x;
                float e1y = p1.y - p0.y;
                float e1z = p1.z - p0.z;

                float e2x = p2.x - p0.x;
                float e2y = p2.y - p0.y;
                float e2z = p2.z - p0.z;

                // 面法线 (Cross Product e1 x e2)
                float cpx = e1y * e2z - e1z * e2y;
                float cpy = e1z * e2x - e1x * e2z;
                float cpz = e1x * e2y - e1y * e2x;

                a_verts[i0].norm.x += cpx; a_verts[i0].norm.y += cpy; a_verts[i0].norm.z += cpz;
                a_verts[i1].norm.x += cpx; a_verts[i1].norm.y += cpy; a_verts[i1].norm.z += cpz;
                a_verts[i2].norm.x += cpx; a_verts[i2].norm.y += cpy; a_verts[i2].norm.z += cpz;
            }
        }

        for (auto& v : a_verts) {
            float lenSq = v.norm.x * v.norm.x + v.norm.y * v.norm.y + v.norm.z * v.norm.z;
            if (lenSq > 1e-6f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                v.norm.x *= invLen;
                v.norm.y *= invLen;
                v.norm.z *= invLen;
            } else {
                v.norm = { 0.0f, 0.0f, 1.0f };
            }
        }
    }

    float NifMeshExtractor::HalfToFloat(std::uint16_t a_half)
    {
        // IEEE 754 half-float to single-float conversion
        std::uint32_t sign = (a_half >> 15) & 0x0001;
        std::uint32_t exp  = (a_half >> 10) & 0x001F;
        std::uint32_t mant = a_half & 0x03FF;

        if (exp == 0) {
            if (mant == 0) {
                std::uint32_t result = sign << 31;
                float f;
                std::memcpy(&f, &result, sizeof(f));
                return f;
            } else {
                // Denormalized
                while (!(mant & 0x0400)) {
                    mant <<= 1;
                    exp--;
                }
                exp++;
                mant &= ~0x0400;
            }
        } else if (exp == 31) {
            std::uint32_t result = (sign << 31) | 0x7F800000 | (mant << 13);
            float f;
            std::memcpy(&f, &result, sizeof(f));
            return f;
        }

        exp = exp + (127 - 15);
        mant = mant << 13;
        std::uint32_t result = (sign << 31) | (exp << 23) | mant;
        float f;
        std::memcpy(&f, &result, sizeof(f));
        return f;
    }

    ExtractedMesh NifMeshExtractor::LoadDefaultPartNif(PreviewPart a_part, BodySex a_sex)
    {
        std::vector<std::string> candidatePaths;
        switch (a_part) {
        case PreviewPart::kFace:
            if (a_sex == BodySex::kMale) {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/malehead.nif",
                    "Data/meshes/actors/character/character assets/malehead.nif",
                    "meshes/actors/character/character assets/malehead.nif",
                    "Data/Meshes/KL/High Poly Head/malehead.nif",
                    "Data/meshes/KL/High Poly Head/malehead.nif",
                    "meshes/KL/High Poly Head/malehead.nif",
                    "Data/Meshes/actors/character/character assets/femalehead.nif",
                    "Data/meshes/actors/character/character assets/femalehead.nif",
                    "meshes/actors/character/character assets/femalehead.nif",
                    "Data/Meshes/KL/High Poly Head/femalehead.nif",
                    "Data/meshes/KL/High Poly Head/femalehead.nif",
                    "meshes/KL/High Poly Head/femalehead.nif"
                };
            } else {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/femalehead.nif",
                    "Data/meshes/actors/character/character assets/femalehead.nif",
                    "meshes/actors/character/character assets/femalehead.nif",
                    "Data/Meshes/KL/High Poly Head/femalehead.nif",
                    "Data/meshes/KL/High Poly Head/femalehead.nif",
                    "meshes/KL/High Poly Head/femalehead.nif",
                    "Data/Meshes/actors/character/character assets/malehead.nif",
                    "Data/meshes/actors/character/character assets/malehead.nif",
                    "meshes/actors/character/character assets/malehead.nif",
                    "Data/Meshes/KL/High Poly Head/malehead.nif",
                    "Data/meshes/KL/High Poly Head/malehead.nif",
                    "meshes/KL/High Poly Head/malehead.nif"
                };
            }
            break;

        case PreviewPart::kHands:
            if (a_sex == BodySex::kMale) {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/malehands_1.nif",
                    "Data/Meshes/actors/character/character assets/malehands_0.nif",
                    "Data/meshes/actors/character/character assets/malehands_1.nif",
                    "Data/meshes/actors/character/character assets/malehands_0.nif",
                    "meshes/actors/character/character assets/malehands_1.nif",
                    "meshes/actors/character/character assets/malehands_0.nif",
                    "Data/Meshes/actors/character/character assets/femalehands_1.nif",
                    "Data/meshes/actors/character/character assets/femalehands_1.nif"
                };
            } else {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/femalehands_1.nif",
                    "Data/Meshes/actors/character/character assets/femalehands_0.nif",
                    "Data/meshes/actors/character/character assets/femalehands_1.nif",
                    "Data/meshes/actors/character/character assets/femalehands_0.nif",
                    "meshes/actors/character/character assets/femalehands_1.nif",
                    "meshes/actors/character/character assets/femalehands_0.nif",
                    "Data/Meshes/actors/character/character assets/malehands_1.nif",
                    "Data/meshes/actors/character/character assets/malehands_1.nif"
                };
            }
            break;

        case PreviewPart::kFeet:
            if (a_sex == BodySex::kMale) {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/malefeet_1.nif",
                    "Data/Meshes/actors/character/character assets/malefeet_0.nif",
                    "Data/meshes/actors/character/character assets/malefeet_1.nif",
                    "Data/meshes/actors/character/character assets/malefeet_0.nif",
                    "meshes/actors/character/character assets/malefeet_1.nif",
                    "meshes/actors/character/character assets/malefeet_0.nif",
                    "Data/Meshes/actors/character/character assets/femalefeet_1.nif",
                    "Data/meshes/actors/character/character assets/femalefeet_1.nif"
                };
            } else {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/femalefeet_1.nif",
                    "Data/Meshes/actors/character/character assets/femalefeet_0.nif",
                    "Data/meshes/actors/character/character assets/femalefeet_1.nif",
                    "Data/meshes/actors/character/character assets/femalefeet_0.nif",
                    "meshes/actors/character/character assets/femalefeet_1.nif",
                    "meshes/actors/character/character assets/femalefeet_0.nif",
                    "Data/Meshes/actors/character/character assets/malefeet_1.nif",
                    "Data/meshes/actors/character/character assets/malefeet_1.nif"
                };
            }
            break;

        case PreviewPart::kBody:
        default:
            if (a_sex == BodySex::kMale) {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/malebody_1.nif",
                    "Data/Meshes/actors/character/character assets/malebody_0.nif",
                    "Data/meshes/actors/character/character assets/malebody_1.nif",
                    "Data/meshes/actors/character/character assets/malebody_0.nif",
                    "meshes/actors/character/character assets/malebody_1.nif",
                    "meshes/actors/character/character assets/malebody_0.nif",
                    "Data/Meshes/actors/character/character assets/femalebody_1.nif",
                    "Data/meshes/actors/character/character assets/femalebody_1.nif"
                };
            } else {
                candidatePaths = {
                    "Data/Meshes/actors/character/character assets/femalebody_1.nif",
                    "Data/Meshes/actors/character/character assets/femalebody_0.nif",
                    "Data/meshes/actors/character/character assets/femalebody_1.nif",
                    "Data/meshes/actors/character/character assets/femalebody_0.nif",
                    "meshes/actors/character/character assets/femalebody_1.nif",
                    "meshes/actors/character/character assets/femalebody_0.nif",
                    "Data/Meshes/actors/character/character assets/malebody_1.nif",
                    "Data/meshes/actors/character/character assets/malebody_1.nif"
                };
            }
            break;
        }

        std::error_code ec;
        for (const auto& path : candidatePaths) {
            if (fs::exists(path, ec)) {
                auto mesh = LoadFromNif(path);
                if (mesh.success && !mesh.vertices.empty()) {
                    return mesh;
                }
            }
        }

        return {};
    }

    ExtractedMesh NifMeshExtractor::LoadFromNif(const std::string& a_filePath)
    {
        ExtractedMesh result;
        result.sourcePath = a_filePath;
        result.success = false;

        std::ifstream file(a_filePath, std::ios::binary);
        if (!file.is_open()) {
            return result;
        }

        file.seekg(0, std::ios::end);
        auto endPos = file.tellg();
        if (endPos <= 0 || static_cast<std::size_t>(endPos) < sizeof(NifHeaderPrefix)) {
            return result;
        }
        std::size_t fileSize = static_cast<std::size_t>(endPos);
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> buffer(fileSize);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

        const auto* prefix = reinterpret_cast<const NifHeaderPrefix*>(buffer.data());
        if (std::memcmp(prefix->magic, "Gamebryo File Format, Version 20.2.0.7\n", 39) != 0) {
            return result;
        }

        std::size_t pos = sizeof(NifHeaderPrefix);
        const std::uint32_t numBlocks = prefix->numBlocks;
        const std::uint32_t bsVersion = prefix->bsVersion;

        // 安全防范：每个 Block 至少需要 2 字节类型索引 + 4 字节大小，numBlocks 绝不可能超出文件上限
        if (static_cast<std::uint64_t>(numBlocks) * 6ULL > fileSize) {
            SKSE::log::warn("[RENDER] NifMeshExtractor: invalid numBlocks {} exceeds file capacity", numBlocks);
            return result;
        }

        // 跳过 3 个 export/process 字符串
        for (int i = 0; i < 3; ++i) {
            if (pos >= fileSize) return result;
            std::uint8_t slen = buffer[pos++];
            pos += slen;
        }

        if (pos + 2 > fileSize) return result;
        std::uint16_t numBlockTypes = *reinterpret_cast<const std::uint16_t*>(&buffer[pos]);
        pos += 2;

        std::vector<std::string> blockTypes;
        for (std::uint16_t i = 0; i < numBlockTypes; ++i) {
            if (pos + 4 > fileSize) return result;
            std::uint32_t tlen = *reinterpret_cast<const std::uint32_t*>(&buffer[pos]);
            pos += 4;
            if (pos + tlen > fileSize) return result;
            blockTypes.emplace_back(reinterpret_cast<const char*>(&buffer[pos]), tlen);
            pos += tlen;
        }

        const std::size_t typeIndicesSize = static_cast<std::size_t>(numBlocks) * 2;
        if (pos + typeIndicesSize > fileSize) return result;
        const auto* blockTypeIndices = reinterpret_cast<const std::uint16_t*>(&buffer[pos]);
        pos += typeIndicesSize;

        const std::size_t blockSizesSize = static_cast<std::size_t>(numBlocks) * 4;
        if (pos + blockSizesSize > fileSize) return result;
        const auto* blockSizes = reinterpret_cast<const std::uint32_t*>(&buffer[pos]);
        pos += blockSizesSize;

        if (pos + 8 > fileSize) return result;
        std::uint32_t numStrings = *reinterpret_cast<const std::uint32_t*>(&buffer[pos]);
        pos += 8;

        std::vector<std::string> strings;
        for (std::uint32_t i = 0; i < numStrings; ++i) {
            if (pos + 4 > fileSize) return result;
            std::uint32_t slen = *reinterpret_cast<const std::uint32_t*>(&buffer[pos]);
            pos += 4;
            if (pos + slen > fileSize) return result;
            strings.emplace_back(reinterpret_cast<const char*>(&buffer[pos]), slen);
            pos += slen;
        }

        if (pos + 4 > fileSize) return result;
        pos += 4; // num_groups

        // 计算各个 Block 的偏移量
        std::vector<std::size_t> blockOffsets(numBlocks);
        std::size_t curOffset = pos;
        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            blockOffsets[i] = curOffset;
            curOffset += blockSizes[i];
            if (curOffset > fileSize) return result;
        }

        // 1. 优先检测 BSDynamicTriShape (Skyrim SE/AE 面部/头部网格结构，其三维顶点存放在动态数据中)
        std::int32_t dynShapeBlock = -1;
        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            if (blockTypeIndices[i] >= blockTypes.size()) continue;
            const auto& type = blockTypes[blockTypeIndices[i]];
            if (type == "BSDynamicTriShape") {
                dynShapeBlock = static_cast<std::int32_t>(i);
                break;
            }
        }

        if (dynShapeBlock != -1) {
            // 寻找同 NIF 中的 NiSkinPartition 蒙皮与三角形块
            std::int32_t skinPartBlock = -1;
            for (std::uint32_t i = 0; i < numBlocks; ++i) {
                if (blockTypeIndices[i] >= blockTypes.size()) continue;
                if (blockTypes[blockTypeIndices[i]] == "NiSkinPartition") {
                    skinPartBlock = static_cast<std::int32_t>(i);
                    break;
                }
            }

            const std::uint8_t* dynData = &buffer[blockOffsets[dynShapeBlock]];
            std::size_t dynSize = blockSizes[dynShapeBlock];
            const std::uint8_t* partData = (skinPartBlock != -1) ? &buffer[blockOffsets[skinPartBlock]] : nullptr;
            std::size_t partSize = (skinPartBlock != -1) ? blockSizes[skinPartBlock] : 0;

            if (ParseDynamicTriShape(dynData, dynSize, partData, partSize, bsVersion, result.vertices, result.indices)) {
                fs::path p(a_filePath);
                result.shapeName = p.stem().string() + " (BSDynamicTriShape)";
                result.success = true;
                return result;
            }
        }

        // 2. 寻找最优的身体几何体 Block (优先找最大的 NiSkinPartition)
        std::int32_t targetPartitionBlock = -1;
        std::size_t largestPartitionSize = 0;

        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            if (blockTypeIndices[i] >= blockTypes.size()) continue;
            const auto& type = blockTypes[blockTypeIndices[i]];
            if (type == "NiSkinPartition") {
                if (blockSizes[i] > largestPartitionSize) {
                    largestPartitionSize = blockSizes[i];
                    targetPartitionBlock = static_cast<std::int32_t>(i);
                }
            }
        }

        if (targetPartitionBlock != -1) {
            fs::path p(a_filePath);
            result.shapeName = p.stem().string() + " (NiSkinPartition)";
            const std::uint8_t* pData = &buffer[blockOffsets[targetPartitionBlock]];
            std::size_t pSize = blockSizes[targetPartitionBlock];

            if (ParseNiSkinPartition(pData, pSize, bsVersion, result.vertices, result.indices)) {
                result.success = true;
                return result;
            }
        }

        // 备选路径：未蒙皮的直接 BSTriShape 遍历
        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            if (blockTypeIndices[i] >= blockTypes.size()) continue;
            const auto& type = blockTypes[blockTypeIndices[i]];
            if (type == "BSTriShape") {
                const std::uint8_t* bData = &buffer[blockOffsets[i]];
                std::size_t bSize = blockSizes[i];

                if (bSize > 120) {
                    // 读取 BSTriShape 头部
                    std::size_t p = 0;
                    if (p + 8 > bSize) continue;
                    std::int32_t nameIdx = *reinterpret_cast<const std::int32_t*>(&bData[p]);
                    p += 4;
                    std::uint32_t numExtra = *reinterpret_cast<const std::uint32_t*>(&bData[p]);
                    p += 4;

                    std::size_t headerRemaining = 4 * numExtra + 92; // 92 字节对应 flags, transform, sphere, properties
                    if (p + headerRemaining + 16 > bSize) continue;
                    p += headerRemaining;

                    std::uint64_t vDesc = *reinterpret_cast<const std::uint64_t*>(&bData[p]);
                    p += 8;
                    std::uint16_t numTri = *reinterpret_cast<const std::uint16_t*>(&bData[p]);
                    p += 2;
                    std::uint16_t numVert = *reinterpret_cast<const std::uint16_t*>(&bData[p]);
                    p += 2;
                    std::uint32_t dataSz = *reinterpret_cast<const std::uint32_t*>(&bData[p]);
                    p += 4;

                    if (numTri > 0 && numVert > 0 && p < bSize) {
                        if (nameIdx >= 0 && nameIdx < static_cast<std::int32_t>(strings.size())) {
                            result.shapeName = strings[nameIdx];
                        } else {
                            fs::path fp(a_filePath);
                            result.shapeName = fp.stem().string() + " (BSTriShape)";
                        }
                        if (ParseDirectTriShape(&bData[p], bSize - p, numTri, numVert, vDesc, result.vertices, result.indices)) {
                            result.success = true;
                            return result;
                        }
                    }
                }
            }
        }

        return result;
    }

    bool NifMeshExtractor::ParseDynamicTriShape(
        const std::uint8_t* a_shapeData,
        std::size_t a_shapeSize,
        const std::uint8_t* a_partitionData,
        std::size_t a_partitionSize,
        std::uint32_t a_bsVersion,
        std::vector<MannequinVertex>& a_outVerts,
        std::vector<std::uint32_t>& a_outIndices)
    {
        if (!a_shapeData || a_shapeSize < 124) return false;

        std::size_t p = 0;
        if (p + 8 > a_shapeSize) return false;
        std::int32_t nameIdx = *reinterpret_cast<const std::int32_t*>(&a_shapeData[p]);
        p += 4;
        std::uint32_t numExtra = *reinterpret_cast<const std::uint32_t*>(&a_shapeData[p]);
        p += 4;

        std::size_t headerRemaining = 4 * numExtra + 92;
        if (p + headerRemaining + 16 > a_shapeSize) return false;
        p += headerRemaining;

        std::uint64_t vDesc = *reinterpret_cast<const std::uint64_t*>(&a_shapeData[p]);
        p += 8;
        std::uint16_t numTri = *reinterpret_cast<const std::uint16_t*>(&a_shapeData[p]);
        p += 2;
        std::uint16_t numVert = *reinterpret_cast<const std::uint16_t*>(&a_shapeData[p]);
        p += 2;
        std::uint32_t dataSz = *reinterpret_cast<const std::uint32_t*>(&a_shapeData[p]);
        p += 4;

        p += dataSz + static_cast<std::size_t>(numTri) * 6;
        if (p + 4 > a_shapeSize) return false;

        std::uint32_t dynDataSz = *reinterpret_cast<const std::uint32_t*>(&a_shapeData[p]);
        p += 4;
        if (dynDataSz == 0 && (p + 4 <= a_shapeSize)) {
            dynDataSz = *reinterpret_cast<const std::uint32_t*>(&a_shapeData[p]);
            p += 4;
        }

        std::uint32_t vertCount = dynDataSz / 16;
        if (vertCount == 0 || (p + static_cast<std::size_t>(vertCount) * 16 > a_shapeSize)) {
            return false;
        }

        // 提取顶点位置并计算包围盒
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;
        float minZ = 1e9f, maxZ = -1e9f;

        const auto* dynVerts = reinterpret_cast<const float*>(&a_shapeData[p]);
        for (std::uint32_t i = 0; i < vertCount; ++i) {
            float x = dynVerts[i * 4 + 0];
            float y = dynVerts[i * 4 + 1];
            float z = dynVerts[i * 4 + 2];
            if (!std::isnan(x) && !std::isinf(x) &&
                !std::isnan(y) && !std::isinf(y) &&
                !std::isnan(z) && !std::isinf(z)) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
                minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
            }
        }

        if (minX > maxX || minY > maxY || minZ > maxZ) {
            return false;
        }

        float dimX = std::max(0.1f, maxX - minX);
        float dimY = std::max(0.1f, maxY - minY);
        float dimZ = std::max(0.1f, maxZ - minZ);
        float maxDim = std::max({ dimX, dimY, dimZ });
        if (maxDim < 1.0f) {
            return false;
        }
        float scale = 1.9f / maxDim;

        float centerX = (minX + maxX) * 0.5f;
        float centerY_nif = (minY + maxY) * 0.5f;
        float centerZ = (minZ + maxZ) * 0.5f;

        // 尝试从 NiSkinPartition 提取 UV
        std::vector<DirectX::XMFLOAT2> uvs(vertCount, DirectX::XMFLOAT2(0.5f, 0.5f));
        if (a_partitionData && a_partitionSize > 20) {
            std::uint32_t partDataSize = *reinterpret_cast<const std::uint32_t*>(&a_partitionData[4]);
            std::uint32_t partStride = *reinterpret_cast<const std::uint32_t*>(&a_partitionData[8]);
            if (partStride >= 4) {
                const std::uint8_t* vPtr = a_partitionData + 20;
                for (std::uint32_t i = 0; i < vertCount; ++i) {
                    if (20 + (i + 1) * partStride <= a_partitionSize && (i + 1) * partStride <= partDataSize) {
                        std::uint16_t uRaw = *reinterpret_cast<const std::uint16_t*>(vPtr + i * partStride + 0);
                        std::uint16_t vRaw = *reinterpret_cast<const std::uint16_t*>(vPtr + i * partStride + 2);
                        float u = HalfToFloat(uRaw);
                        float v = HalfToFloat(vRaw);
                        if (!std::isnan(u) && !std::isinf(u)) uvs[i].x = u;
                        if (!std::isnan(v) && !std::isinf(v)) uvs[i].y = v;
                    }
                }
            }
        }

        a_outVerts.clear();
        a_outVerts.reserve(vertCount);
        for (std::uint32_t i = 0; i < vertCount; ++i) {
            float x = dynVerts[i * 4 + 0];
            float y = dynVerts[i * 4 + 1];
            float z = dynVerts[i * 4 + 2];
            if (std::isnan(x) || std::isinf(x)) x = centerX;
            if (std::isnan(y) || std::isinf(y)) y = centerY_nif;
            if (std::isnan(z) || std::isinf(z)) z = centerZ;

            MannequinVertex vert{};
            vert.pos.x = (x - centerX) * scale;
            vert.pos.y = (z - centerZ) * scale;
            vert.pos.z = (y - centerY_nif) * scale; // 正面朝前 (+Z)
            vert.uv = uvs[i];
            vert.norm = { vert.pos.x, 0.2f, vert.pos.z };
            a_outVerts.push_back(vert);
        }

        // 提取三角形索引
        a_outIndices.clear();
        if (a_partitionData && a_partitionSize > 20) {
            std::uint32_t numPartitions = *reinterpret_cast<const std::uint32_t*>(&a_partitionData[0]);
            std::uint32_t partDataSize = *reinterpret_cast<const std::uint32_t*>(&a_partitionData[4]);
            std::size_t triOffset = 20 + partDataSize;

            for (std::uint32_t pIdx = 0; pIdx < numPartitions; ++pIdx) {
                if (triOffset + 10 > a_partitionSize) break;

                std::uint16_t partVerts = *reinterpret_cast<const std::uint16_t*>(&a_partitionData[triOffset]);
                std::uint16_t partTriangles = *reinterpret_cast<const std::uint16_t*>(&a_partitionData[triOffset + 2]);
                std::uint16_t partBones = *reinterpret_cast<const std::uint16_t*>(&a_partitionData[triOffset + 4]);
                std::uint16_t partStrips = *reinterpret_cast<const std::uint16_t*>(&a_partitionData[triOffset + 6]);
                std::uint16_t partWeights = *reinterpret_cast<const std::uint16_t*>(&a_partitionData[triOffset + 8]);
                triOffset += 10;

                triOffset += partBones * 2;

                if (triOffset >= a_partitionSize) break;
                uint8_t hasVMap = a_partitionData[triOffset++];
                if (hasVMap) {
                    if (triOffset + partVerts * 2 > a_partitionSize) break;
                    triOffset += partVerts * 2;
                }

                if (triOffset >= a_partitionSize) break;
                uint8_t hasWeights = a_partitionData[triOffset++];
                if (hasWeights) {
                    if (triOffset + static_cast<std::size_t>(partVerts) * partWeights * 4 > a_partitionSize) break;
                    triOffset += static_cast<std::size_t>(partVerts) * partWeights * 4;
                }

                if (triOffset >= a_partitionSize) break;
                uint8_t hasFaces = a_partitionData[triOffset++];
                if (hasFaces && partStrips == 0) {
                    if (triOffset + static_cast<std::size_t>(partTriangles) * 6 > a_partitionSize) break;
                    const auto* triList = reinterpret_cast<const uint16_t*>(&a_partitionData[triOffset]);
                    triOffset += static_cast<std::size_t>(partTriangles) * 3 * 2;

                    for (std::uint16_t t = 0; t < partTriangles; ++t) {
                        uint32_t v0 = triList[t * 3 + 0];
                        uint32_t v1 = triList[t * 3 + 1];
                        uint32_t v2 = triList[t * 3 + 2];
                        if (v0 < vertCount && v1 < vertCount && v2 < vertCount) {
                            a_outIndices.push_back(v0);
                            a_outIndices.push_back(v1);
                            a_outIndices.push_back(v2);
                        }
                    }
                }

                if (triOffset < a_partitionSize) {
                    uint8_t hasBIdx = a_partitionData[triOffset++];
                    if (hasBIdx) {
                        if (triOffset + static_cast<std::size_t>(partVerts) * partWeights > a_partitionSize) break;
                        triOffset += static_cast<std::size_t>(partVerts) * partWeights;
                    }
                }

                if (a_bsVersion >= 100) {
                    std::size_t sseFooter = 1 + 1 + 8 + static_cast<std::size_t>(partTriangles) * 6;
                    if (triOffset + sseFooter <= a_partitionSize) {
                        triOffset += sseFooter;
                    }
                }
            }
        }

        if (!a_outVerts.empty() && !a_outIndices.empty()) {
            ComputeSmoothNormals(a_outVerts, a_outIndices);
            return true;
        }

        return false;
    }

    bool NifMeshExtractor::ParseNiSkinPartition(
        const std::uint8_t* a_data,
        std::size_t a_size,
        std::uint32_t a_bsVersion,
        std::vector<MannequinVertex>& a_outVerts,
        std::vector<std::uint32_t>& a_outIndices)
    {
        if (a_size < 20) return false;

        // NiSkinPartition Header:
        // 0..3: num_partitions (uint32)
        // 4..7: data_size (uint32)
        // 8..11: vertex_stride (uint32, typically 32)
        // 12..19: vertex_flags (uint64)
        std::uint32_t numPartitions = *reinterpret_cast<const std::uint32_t*>(&a_data[0]);
        std::uint32_t dataSize = *reinterpret_cast<const std::uint32_t*>(&a_data[4]);
        std::uint32_t vertexStride = *reinterpret_cast<const std::uint32_t*>(&a_data[8]);

        if (vertexStride < 16 || dataSize == 0 || static_cast<std::size_t>(dataSize) > a_size - 20) {
            return false;
        }

        std::uint32_t numVertices = dataSize / vertexStride;
        if (numVertices == 0) return false;

        a_outVerts.clear();
        a_outVerts.reserve(numVertices);

        // 寻找中心包围盒以做自适应居中与缩放
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;
        float minZ = 1e9f, maxZ = -1e9f;

        const std::uint8_t* vPtr = a_data + 20;
        for (std::uint32_t i = 0; i < numVertices; ++i) {
            const auto* v = vPtr + i * vertexStride;
            float x = *reinterpret_cast<const float*>(v);
            float y = *reinterpret_cast<const float*>(v + 4);
            float z = *reinterpret_cast<const float*>(v + 8);

            if (!std::isnan(x) && !std::isinf(x) &&
                !std::isnan(y) && !std::isinf(y) &&
                !std::isnan(z) && !std::isinf(z)) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
                minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
            }
        }

        if (minX > maxX || minY > maxY || minZ > maxZ) {
            minX = -1.0f; maxX = 1.0f;
            minY = -1.0f; maxY = 1.0f;
            minZ = -1.0f; maxZ = 1.0f;
        }

        // 老滚世界的单位：X向右, Y向前, Z向上
        // 计算几何体在各轴的中心点，使得无论是身体、头部、双手还是双脚都能居中在视口原点 (0, 0, 0)
        float centerX = (minX + maxX) * 0.5f;
        float centerY_nif = (minY + maxY) * 0.5f;
        float centerZ = (minZ + maxZ) * 0.5f;

        float dimX = std::max(0.1f, maxX - minX);
        float dimY = std::max(0.1f, maxY - minY);
        float dimZ = std::max(0.1f, maxZ - minZ);
        float maxDim = std::max({ dimX, dimY, dimZ });
        if (maxDim < 1.0f) {
            return false;
        }
        float scale = 1.9f / maxDim; // 缩放到合适视口尺度

        // 判断 UV 偏移：Skyrim SE (vertexStride >= 28) 包含 bitangent/unusedW (4 字节)，UV 在 offset 16
        // 若较短 (vertexStride < 28) 则 UV 在 offset 12
        const std::size_t uvOffset = (vertexStride >= 28) ? 16 : 12;

        for (std::uint32_t i = 0; i < numVertices; ++i) {
            const auto* v = vPtr + i * vertexStride;
            float nx = *reinterpret_cast<const float*>(v);
            float ny = *reinterpret_cast<const float*>(v + 4);
            float nz = *reinterpret_cast<const float*>(v + 8);

            // 读取半精度 UV (float16)
            std::uint16_t uRaw = *reinterpret_cast<const std::uint16_t*>(v + uvOffset);
            std::uint16_t vRaw = *reinterpret_cast<const std::uint16_t*>(v + uvOffset + 2);

            float u = HalfToFloat(uRaw);
            float vCoord = HalfToFloat(vRaw);

            // 如果半精度解析出非正常值则安全兜底
            if (std::isnan(u) || std::isinf(u)) u = 0.5f;
            if (std::isnan(vCoord) || std::isinf(vCoord)) vCoord = 0.5f;

            // 异常顶点坐标防溢出
            if (std::isnan(nx) || std::isinf(nx)) nx = centerX;
            if (std::isnan(ny) || std::isinf(ny)) ny = centerY_nif;
            if (std::isnan(nz) || std::isinf(nz)) nz = centerZ;

            MannequinVertex vert{};
            // 坐标轴变换并自适应居中：老滚 (X右, Y前, Z上) -> 视口 (X右, Y上, Z前)
            vert.pos.x = (nx - centerX) * scale;
            vert.pos.y = (nz - centerZ) * scale;
            vert.pos.z = (ny - centerY_nif) * scale;

            // 解析顶点法线 (ByteVector3 位于 offset 20，若不可用则用向外辐射法线兜底)
            if (vertexStride >= 24) {
                float normX = (static_cast<float>(v[20]) / 127.5f) - 1.0f;
                float normY = (static_cast<float>(v[21]) / 127.5f) - 1.0f;
                float normZ = (static_cast<float>(v[22]) / 127.5f) - 1.0f;
                vert.norm.x = normX;
                vert.norm.y = normZ;
                vert.norm.z = normY;
                float nlen = std::sqrt(vert.norm.x * vert.norm.x + vert.norm.y * vert.norm.y + vert.norm.z * vert.norm.z);
                if (nlen > 0.001f && !std::isnan(nlen)) {
                    vert.norm.x /= nlen;
                    vert.norm.y /= nlen;
                    vert.norm.z /= nlen;
                } else {
                    vert.norm = { vert.pos.x, 0.2f, vert.pos.z };
                }
            } else {
                vert.norm = { vert.pos.x, 0.2f, vert.pos.z };
            }

            vert.uv.x = u;
            vert.uv.y = vCoord;

            a_outVerts.push_back(vert);
        }

        // 解析三角形列表 (位于顶点数据之后)
        std::size_t triOffset = 20 + dataSize;
        a_outIndices.clear();

        // 遍历所有 Partitions 读取三角形
        for (std::uint32_t p = 0; p < numPartitions; ++p) {
            if (triOffset + 10 > a_size) break;

            std::uint16_t partVerts = *reinterpret_cast<const std::uint16_t*>(&a_data[triOffset]);
            std::uint16_t partTriangles = *reinterpret_cast<const std::uint16_t*>(&a_data[triOffset + 2]);
            std::uint16_t partBones = *reinterpret_cast<const std::uint16_t*>(&a_data[triOffset + 4]);
            std::uint16_t partStrips = *reinterpret_cast<const std::uint16_t*>(&a_data[triOffset + 6]);
            std::uint16_t partWeights = *reinterpret_cast<const std::uint16_t*>(&a_data[triOffset + 8]);
            triOffset += 10;

            triOffset += partBones * 2; // bones list

            if (triOffset >= a_size) break;
            uint8_t hasVMap = a_data[triOffset++];
            const uint16_t* vMap = nullptr;
            if (hasVMap) {
                if (triOffset + partVerts * 2 > a_size) break;
                vMap = reinterpret_cast<const uint16_t*>(&a_data[triOffset]);
                triOffset += partVerts * 2;
            }

            if (triOffset >= a_size) break;
            uint8_t hasWeights = a_data[triOffset++];
            if (hasWeights) {
                if (triOffset + static_cast<std::size_t>(partVerts) * partWeights * 4 > a_size) break;
                triOffset += static_cast<std::size_t>(partVerts) * partWeights * 4;
            }

            if (triOffset >= a_size) break;
            uint8_t hasFaces = a_data[triOffset++];
            if (hasFaces && partStrips == 0) {
                if (triOffset + static_cast<std::size_t>(partTriangles) * 6 > a_size) break;
                const auto* triList = reinterpret_cast<const uint16_t*>(&a_data[triOffset]);
                triOffset += static_cast<std::size_t>(partTriangles) * 3 * 2;

                for (std::uint16_t t = 0; t < partTriangles; ++t) {
                    uint16_t i0 = triList[t * 3 + 0];
                    uint16_t i1 = triList[t * 3 + 1];
                    uint16_t i2 = triList[t * 3 + 2];

                    // Skyrim SE/AE (bsVersion >= 100) 下共享全局顶点缓冲，三角形索引本来就是全局顶点索引，
                    // 绝不能再以局部索引查 vMap 二次映射，否则会导致顶点乱飞、撕碎网格。
                    uint32_t v0 = i0;
                    uint32_t v1 = i1;
                    uint32_t v2 = i2;

                    if (v0 < numVertices && v1 < numVertices && v2 < numVertices) {
                        a_outIndices.push_back(v0);
                        a_outIndices.push_back(v1);
                        a_outIndices.push_back(v2);
                    }
                }
            }

            if (triOffset < a_size) {
                uint8_t hasBIdx = a_data[triOffset++];
                if (hasBIdx) {
                    if (triOffset + static_cast<std::size_t>(partVerts) * partWeights > a_size) break;
                    triOffset += static_cast<std::size_t>(partVerts) * partWeights;
                }
            }

            // Skyrim SE / AE (bsVersion >= 100): 跳过 partition 尾部附加字段
            // LOD Level (1 byte) + Global VB (1 byte) + Vertex Desc (8 bytes) + Triangles Copy (partTriangles * 6 bytes)
            if (a_bsVersion >= 100) {
                std::size_t sseFooter = 1 + 1 + 8 + static_cast<std::size_t>(partTriangles) * 6;
                if (triOffset + sseFooter <= a_size) {
                    triOffset += sseFooter;
                }
            }
        }

        if (!a_outVerts.empty() && !a_outIndices.empty()) {
            ComputeSmoothNormals(a_outVerts, a_outIndices);
            return true;
        }
        return false;
    }

    bool NifMeshExtractor::ParseDirectTriShape(
        const std::uint8_t* a_data,
        std::size_t a_size,
        std::uint32_t a_numTriangles,
        std::uint32_t a_numVertices,
        std::uint64_t /*a_vertexDesc*/,
        std::vector<MannequinVertex>& a_outVerts,
        std::vector<std::uint32_t>& a_outIndices)
    {
        if (a_numVertices == 0 || a_numTriangles == 0) return false;

        std::uint64_t totalTriPoints = static_cast<std::uint64_t>(a_numTriangles) * 3ULL;
        if (totalTriPoints > (std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t))) return false;
        std::size_t triBytes = static_cast<std::size_t>(totalTriPoints * sizeof(std::uint16_t));
        if (triBytes > a_size) return false;

        std::size_t vertexBytesTotal = a_size - triBytes;
        std::size_t stride = vertexBytesTotal / a_numVertices;
        if (stride < 16) return false;

        // 计算中心与包围盒做自适应居中与缩放
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;
        float minZ = 1e9f, maxZ = -1e9f;

        const std::uint8_t* vPtr = a_data;
        for (std::uint32_t i = 0; i < a_numVertices; ++i) {
            const auto* v = vPtr + i * stride;
            float x = *reinterpret_cast<const float*>(v);
            float y = *reinterpret_cast<const float*>(v + 4);
            float z = *reinterpret_cast<const float*>(v + 8);

            if (!std::isnan(x) && !std::isinf(x) &&
                !std::isnan(y) && !std::isinf(y) &&
                !std::isnan(z) && !std::isinf(z)) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
                minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
            }
        }

        if (minX > maxX || minY > maxY || minZ > maxZ) {
            minX = -1.0f; maxX = 1.0f;
            minY = -1.0f; maxY = 1.0f;
            minZ = -1.0f; maxZ = 1.0f;
        }

        float centerX = (minX + maxX) * 0.5f;
        float centerY_nif = (minY + maxY) * 0.5f;
        float centerZ = (minZ + maxZ) * 0.5f;

        float dimX = std::max(0.1f, maxX - minX);
        float dimY = std::max(0.1f, maxY - minY);
        float dimZ = std::max(0.1f, maxZ - minZ);
        float maxDim = std::max({ dimX, dimY, dimZ });
        if (maxDim < 1.0f) return false;
        float scale = 1.9f / maxDim;

        const std::size_t uvOffset = (stride >= 28) ? 16 : 12;

        a_outVerts.clear();
        a_outVerts.reserve(a_numVertices);

        for (std::uint32_t i = 0; i < a_numVertices; ++i) {
            const auto* v = vPtr + i * stride;
            float x = *reinterpret_cast<const float*>(v);
            float y = *reinterpret_cast<const float*>(v + 4);
            float z = *reinterpret_cast<const float*>(v + 8);

            float u = 0.5f;
            float vCoord = 0.5f;
            if (stride >= uvOffset + 4) {
                std::uint16_t uRaw = *reinterpret_cast<const std::uint16_t*>(v + uvOffset);
                std::uint16_t vRaw = *reinterpret_cast<const std::uint16_t*>(v + uvOffset + 2);
                u = HalfToFloat(uRaw);
                vCoord = HalfToFloat(vRaw);
                if (std::isnan(u) || std::isinf(u)) u = 0.5f;
                if (std::isnan(vCoord) || std::isinf(vCoord)) vCoord = 0.5f;
            }

            if (std::isnan(x) || std::isinf(x)) x = centerX;
            if (std::isnan(y) || std::isinf(y)) y = centerY_nif;
            if (std::isnan(z) || std::isinf(z)) z = centerZ;

            MannequinVertex vert{};
            vert.pos = { (x - centerX) * scale, (z - centerZ) * scale, (y - centerY_nif) * scale };
            vert.norm = { vert.pos.x, 0.2f, vert.pos.z };
            if (stride >= 24) {
                float normX = (static_cast<float>(v[20]) / 127.5f) - 1.0f;
                float normY = (static_cast<float>(v[21]) / 127.5f) - 1.0f;
                float normZ = (static_cast<float>(v[22]) / 127.5f) - 1.0f;
                vert.norm = { normX, normZ, normY };
                float nlen = std::sqrt(vert.norm.x * vert.norm.x + vert.norm.y * vert.norm.y + vert.norm.z * vert.norm.z);
                if (nlen > 0.001f && !std::isnan(nlen)) {
                    vert.norm.x /= nlen; vert.norm.y /= nlen; vert.norm.z /= nlen;
                } else {
                    vert.norm = { vert.pos.x, 0.2f, vert.pos.z };
                }
            }
            vert.uv = { u, vCoord };
            a_outVerts.push_back(vert);
        }

        // 三角形位于顶点数据之后
        std::size_t triOffset = a_numVertices * stride;
        const auto* triList = reinterpret_cast<const std::uint16_t*>(a_data + triOffset);
        a_outIndices.clear();
        a_outIndices.reserve(a_numTriangles * 3);
        for (std::uint32_t i = 0; i < a_numTriangles * 3; ++i) {
            uint16_t idx = triList[i];
            if (idx < a_numVertices) {
                a_outIndices.push_back(idx);
            }
        }

        if (!a_outVerts.empty() && !a_outIndices.empty()) {
            ComputeSmoothNormals(a_outVerts, a_outIndices);
            return true;
        }
        return false;
    }
}
