#include "render/NifMeshExtractor.h"
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

    ExtractedMesh NifMeshExtractor::LoadDefaultBodyNif()
    {
        const std::string candidatePaths[] = {
            "Data/Meshes/actors/character/character assets/femalebody_1.nif",
            "Data/Meshes/actors/character/character assets/femalebody_0.nif",
            "Data/meshes/actors/character/character assets/femalebody_1.nif",
            "Data/meshes/actors/character/character assets/femalebody_0.nif"
        };

        for (const auto& path : candidatePaths) {
            if (fs::exists(path)) {
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
        std::size_t fileSize = static_cast<std::size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        if (fileSize < sizeof(NifHeaderPrefix)) {
            return result;
        }

        std::vector<std::uint8_t> buffer(fileSize);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

        const auto* prefix = reinterpret_cast<const NifHeaderPrefix*>(buffer.data());
        if (std::memcmp(prefix->magic, "Gamebryo File Format, Version 20.2.0.7\n", 39) != 0) {
            return result;
        }

        std::size_t pos = sizeof(NifHeaderPrefix);
        const std::uint32_t numBlocks = prefix->numBlocks;

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

        if (pos + numBlocks * 2 > fileSize) return result;
        const auto* blockTypeIndices = reinterpret_cast<const std::uint16_t*>(&buffer[pos]);
        pos += numBlocks * 2;

        if (pos + numBlocks * 4 > fileSize) return result;
        const auto* blockSizes = reinterpret_cast<const std::uint32_t*>(&buffer[pos]);
        pos += numBlocks * 4;

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

        // 寻找最优的身体几何体 Block (优先找名字带 Body / Softbody 的，其次找最大的 NiSkinPartition)
        std::int32_t targetPartitionBlock = -1;
        std::size_t largestPartitionSize = 0;

        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            const auto& type = blockTypes[blockTypeIndices[i]];
            if (type == "NiSkinPartition") {
                if (blockSizes[i] > largestPartitionSize) {
                    largestPartitionSize = blockSizes[i];
                    targetPartitionBlock = static_cast<std::int32_t>(i);
                }
            }
        }

        if (targetPartitionBlock != -1) {
            result.shapeName = "Body (NiSkinPartition)";
            const std::uint8_t* pData = &buffer[blockOffsets[targetPartitionBlock]];
            std::size_t pSize = blockSizes[targetPartitionBlock];

            if (ParseNiSkinPartition(pData, pSize, 0, result.vertices, result.indices)) {
                result.success = true;
                return result;
            }
        }

        // 备选路径：未蒙皮的直接 BSTriShape 遍历
        for (std::uint32_t i = 0; i < numBlocks; ++i) {
            const auto& type = blockTypes[blockTypeIndices[i]];
            if (type == "BSTriShape") {
                const std::uint8_t* bData = &buffer[blockOffsets[i]];
                std::size_t bSize = blockSizes[i];

                if (bSize > 120) {
                    // 读取 BSTriShape 头部
                    std::size_t p = 0;
                    std::int32_t nameIdx = *reinterpret_cast<const std::int32_t*>(&bData[p]);
                    p += 4;
                    std::uint32_t numExtra = *reinterpret_cast<const std::uint32_t*>(&bData[p]);
                    p += 4 + 4 * numExtra + 4 + 4 + 12 + 36 + 4 + 4 + 16 + 12; // 跳过常规属性与包围球
                    std::uint64_t vDesc = *reinterpret_cast<const std::uint64_t*>(&bData[p]);
                    p += 8;
                    std::uint32_t numTri = *reinterpret_cast<const std::uint32_t*>(&bData[p]);
                    p += 4;
                    std::uint16_t numVert = *reinterpret_cast<const std::uint16_t*>(&bData[p]);
                    p += 2;
                    std::uint32_t dataSz = *reinterpret_cast<const std::uint32_t*>(&bData[p]);
                    p += 4;

                    if (numTri > 0 && numVert > 0) {
                        if (nameIdx >= 0 && nameIdx < static_cast<std::int32_t>(strings.size())) {
                            result.shapeName = strings[nameIdx];
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

    bool NifMeshExtractor::ParseNiSkinPartition(
        const std::uint8_t* a_data,
        std::size_t a_size,
        std::uint64_t /*a_vertexDesc*/,
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

        if (vertexStride < 16 || dataSize == 0 || (20 + dataSize) > a_size) {
            return false;
        }

        std::uint32_t numVertices = dataSize / vertexStride;
        if (numVertices == 0) return false;

        a_outVerts.clear();
        a_outVerts.reserve(numVertices);

        // 寻找中心包围盒以做归一化
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;
        float minZ = 1e9f, maxZ = -1e9f;

        const std::uint8_t* vPtr = a_data + 20;
        for (std::uint32_t i = 0; i < numVertices; ++i) {
            const auto* v = vPtr + i * vertexStride;
            float x = *reinterpret_cast<const float*>(v);
            float y = *reinterpret_cast<const float*>(v + 4);
            float z = *reinterpret_cast<const float*>(v + 8);

            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }

        // 老滚世界的单位通常人体高度约 120 单位 (Z: 0 ~ 125)
        // 计算身体中心点 (躯干中心约在 Z 的中上位)
        float centerY = (minZ + maxZ) * 0.55f;
        float sizeY = std::max(1.0f, maxZ - minZ);
        float scale = 2.0f / sizeY; // 缩放到合适视口高度

        for (std::uint32_t i = 0; i < numVertices; ++i) {
            const auto* v = vPtr + i * vertexStride;
            float nx = *reinterpret_cast<const float*>(v);
            float ny = *reinterpret_cast<const float*>(v + 4);
            float nz = *reinterpret_cast<const float*>(v + 8);

            // 读取半精度 UV (float16)
            std::uint16_t uRaw = *reinterpret_cast<const std::uint16_t*>(v + 12);
            std::uint16_t vRaw = *reinterpret_cast<const std::uint16_t*>(v + 14);

            float u = HalfToFloat(uRaw);
            float vCoord = HalfToFloat(vRaw);

            // 如果半精度解析出非正常值则安全兜底
            if (std::isnan(u) || std::isinf(u)) u = 0.5f;
            if (std::isnan(vCoord) || std::isinf(vCoord)) vCoord = 0.5f;

            MannequinVertex vert{};
            // 坐标轴变换：老滚 (X右, Y前, Z上) -> 视口 (X右, Y上, Z前)
            vert.pos.x = nx * scale;
            vert.pos.y = (nz - centerY) * scale;
            vert.pos.z = -ny * scale;

            // 法线
            vert.norm.x = nx;
            vert.norm.y = 0.2f;
            vert.norm.z = -ny;
            float nlen = std::sqrt(vert.norm.x * vert.norm.x + vert.norm.y * vert.norm.y + vert.norm.z * vert.norm.z);
            if (nlen > 0.001f) {
                vert.norm.x /= nlen;
                vert.norm.y /= nlen;
                vert.norm.z /= nlen;
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
                vMap = reinterpret_cast<const uint16_t*>(&a_data[triOffset]);
                triOffset += partVerts * 2;
            }

            if (triOffset >= a_size) break;
            uint8_t hasWeights = a_data[triOffset++];
            if (hasWeights) {
                triOffset += partVerts * partWeights * 4;
            }

            if (triOffset >= a_size) break;
            uint8_t hasFaces = a_data[triOffset++];
            if (hasFaces && partStrips == 0) {
                const auto* triList = reinterpret_cast<const uint16_t*>(&a_data[triOffset]);
                triOffset += partTriangles * 3 * 2;

                for (std::uint16_t t = 0; t < partTriangles; ++t) {
                    uint16_t i0 = triList[t * 3 + 0];
                    uint16_t i1 = triList[t * 3 + 1];
                    uint16_t i2 = triList[t * 3 + 2];

                    uint32_t v0 = (vMap && i0 < partVerts) ? vMap[i0] : i0;
                    uint32_t v1 = (vMap && i1 < partVerts) ? vMap[i1] : i1;
                    uint32_t v2 = (vMap && i2 < partVerts) ? vMap[i2] : i2;

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
                    triOffset += partVerts * partWeights;
                }
            }
        }

        return !a_outVerts.empty() && !a_outIndices.empty();
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
        std::size_t triBytes = a_numTriangles * 3 * sizeof(std::uint16_t);
        if (triBytes > a_size) return false;

        const auto* triList = reinterpret_cast<const std::uint16_t*>(a_data);
        a_outIndices.clear();
        a_outIndices.reserve(a_numTriangles * 3);
        for (std::uint32_t i = 0; i < a_numTriangles * 3; ++i) {
            a_outIndices.push_back(triList[i]);
        }

        std::size_t vOffset = triBytes;
        if (vOffset >= a_size) return false;

        std::size_t remaining = a_size - vOffset;
        std::size_t stride = remaining / a_numVertices;
        if (stride < 12) return false;

        a_outVerts.clear();
        a_outVerts.reserve(a_numVertices);

        const std::uint8_t* vPtr = a_data + vOffset;
        for (std::uint32_t i = 0; i < a_numVertices; ++i) {
            const auto* v = vPtr + i * stride;
            float x = *reinterpret_cast<const float*>(v);
            float y = *reinterpret_cast<const float*>(v + 4);
            float z = *reinterpret_cast<const float*>(v + 8);

            float u = 0.5f;
            float vCoord = 0.5f;
            if (stride >= 16) {
                std::uint16_t uRaw = *reinterpret_cast<const std::uint16_t*>(v + 12);
                std::uint16_t vRaw = *reinterpret_cast<const std::uint16_t*>(v + 14);
                u = HalfToFloat(uRaw);
                vCoord = HalfToFloat(vRaw);
            }

            MannequinVertex vert{};
            vert.pos = { x * 0.02f, z * 0.02f, -y * 0.02f };
            vert.norm = { x, 0.1f, -y };
            vert.uv = { u, vCoord };
            a_outVerts.push_back(vert);
        }

        return true;
    }
}
