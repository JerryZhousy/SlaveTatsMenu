#include "render/MannequinRenderer.h"
#include "render/NifMeshExtractor.h"
#include "render/TextureLoader.h"
#include "data/TargetState.h"
#include <SKSE/SKSE.h>
#include <d3dcompiler.h>
#include <cmath>
#include <numbers>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace stm::render
{
    namespace
    {
        // ---- asynchronous part-mesh loading ---------------------------------
        //
        // NifMeshExtractor::LoadDefaultPartNif is pure filesystem work plus byte
        // parsing: it walks a hard-coded candidate list, reads the file, and
        // decodes BSTriShape / NiSkinPartition. It touches no engine state, which
        // is exactly what makes it safe to move off the render thread.
        //
        // It is also slow enough to matter: a 3BA/CBBE femalebody_1.nif is
        // routinely 5-15 MB and four parts are loaded back to back, so the first
        // frame of the preview (and every sex switch) used to stall for hundreds
        // of milliseconds. The parse now happens on a worker thread; only the
        // D3D buffer creation stays on the render thread, where the device lives.

        constexpr std::size_t kPartCount = static_cast<std::size_t>(PreviewPart::kCount);

        struct MeshLoadResult
        {
            ExtractedMesh parts[kPartCount];
            int           sex = 0;
            bool          ready = false;
        };

        // One worker for the whole process, created on first use and joined in
        // Shutdown(). Detaching instead would let a parse outlive the DLL and
        // write into unmapped memory after SKSE unloads us.
        std::mutex              g_meshMutex;
        std::condition_variable g_meshCv;
        std::thread             g_meshWorker;
        int                     g_meshRequest = -1;   // -1 = nothing queued
        bool                    g_meshStop = false;
        MeshLoadResult          g_meshResult;         // consumed by the render thread

        void MeshWorkerMain()
        {
            for (;;) {
                int sex = 0;
                {
                    std::unique_lock lock(g_meshMutex);
                    g_meshCv.wait(lock, [] { return g_meshStop || g_meshRequest >= 0; });
                    if (g_meshStop) {
                        return;
                    }
                    sex = g_meshRequest;
                    g_meshRequest = -1;
                }

                const auto bodySex = (sex == 1) ? BodySex::kMale : BodySex::kFemale;

                MeshLoadResult local;
                local.sex = sex;
                for (std::size_t i = 0; i < kPartCount; ++i) {
                    local.parts[i] = NifMeshExtractor::LoadDefaultPartNif(static_cast<PreviewPart>(i), bodySex);
                }
                local.ready = true;

                {
                    std::scoped_lock lock(g_meshMutex);
                    // A newer request arrived while we were parsing (the user
                    // toggled sex again): drop this result rather than publish a
                    // body for the wrong sex. The loop picks the newer one up.
                    if (g_meshRequest < 0) {
                        g_meshResult = std::move(local);
                    }
                }
            }
        }

        void StartMeshWorker()
        {
            std::scoped_lock lock(g_meshMutex);
            g_meshStop = false;
            g_meshRequest = -1;
            g_meshResult = {};

            if (!g_meshWorker.joinable()) {
                g_meshWorker = std::thread(MeshWorkerMain);
            }
        }

        void RequestMeshLoad(int a_sex)
        {
            {
                std::scoped_lock lock(g_meshMutex);
                if (g_meshStop) {
                    return;
                }
                g_meshRequest = a_sex;
            }
            g_meshCv.notify_one();
        }

        void StopMeshWorker()
        {
            std::thread worker;
            {
                std::scoped_lock lock(g_meshMutex);
                g_meshStop = true;
                g_meshRequest = -1;
                g_meshResult = {};
                worker = std::move(g_meshWorker);
            }
            g_meshCv.notify_all();

            // Blocks for at most one parse. Shutdown runs on the render thread and
            // the alternative - letting the thread outlive the DLL - is a crash.
            if (worker.joinable()) {
                worker.join();
            }
        }

        // HLSL 内联着色器代码
        const char g_shaderSource[] = R"(
            cbuffer ConstantBuffer : register(b0)
            {
                matrix WorldViewProj;
                matrix World;
                float4 BodyColor;       // 人体基础色与透明度
                float4 TattooColor;     // 纹身叠加颜色与透明度
                float4 CameraPos;       // 相机位置 (世界坐标)
                float4 Params;          // x: hasTattoo, y: isBackArea, z: time, w: unused
                float4 TattooGlow;      // rgb: glow color, a: hasGlow
            };

            struct VS_INPUT
            {
                float3 Pos : POSITION;
                float3 Norm : NORMAL;
                float2 UV : TEXCOORD0;
            };

            struct PS_INPUT
            {
                float4 Pos : SV_POSITION;
                float3 WorldPos : POSITION0;
                float3 WorldNorm : NORMAL0;
                float2 UV : TEXCOORD0;
            };

            Texture2D TattooTexture : register(t0);
            SamplerState SamplerLinear : register(s0);

            PS_INPUT VS_Main(VS_INPUT input)
            {
                PS_INPUT output;
                output.Pos = mul(WorldViewProj, float4(input.Pos, 1.0f));
                output.WorldPos = mul(World, float4(input.Pos, 1.0f)).xyz;
                output.WorldNorm = mul((float3x3)World, input.Norm);
                output.UV = input.UV;
                return output;
            }

            float4 PS_Main(PS_INPUT input) : SV_Target
            {
                // 人体着色一律只用"连续的位置量"。
                // 3BA/CBBE 的身体是由许多互相重叠的附加网格拼成的, 法线在块与块的接缝处有突变,
                // 只要用上 N·L / N·V / 高光, 那些接缝就会变成一片片硬边深浅斑 —— 肉色和全息
                // 两个模式都会中招(你实测过: 关掉体积明暗颜色一致, 打开才有斑块; 与纹身无关)。
                // 位置是连续的, 所以下面这套"高度 + 离中轴远近"的渐变永远不会出硬边。
                float3 wp   = input.WorldPos;                       // 模型已归一化到约 ±1
                float  hgt  = saturate(wp.y * 0.55f + 0.5f);        // 上 -> 下
                float  lat  = saturate(length(wp.xz) * 0.9f);       // 离中轴远近
                float  volume = saturate(0.55f + 0.45f * (0.65f * hgt + 0.35f * (1.0f - lat)));

                // Params.w = 体积明暗强度 0..1。默认 0: 两个模式都是整片纯色。
                float  shadeW = saturate(Params.w);
                float  shadeBase = 1.0f - 0.20f * shadeW;
                float  shadeTerm = shadeBase + 0.20f * shadeW * volume;

                float3 color;
                float alpha;

                if (Params.z > 0.5f) {
                    // 自然肉色: 一整片半透明肉色, 默认几乎纯色
                    color = BodyColor.rgb * shadeTerm;
                    alpha = BodyColor.a;
                } else {
                    // 全息透明蓝: 同一套渐变再叠一点均匀自发光, 读起来像玻璃壳
                    // (原来这里的 N·V 边缘光和 1.23 倍的漫反射范围正是全息模式斑块的来源)
                    color = BodyColor.rgb * shadeTerm + BodyColor.rgb * 0.18f;
                    alpha = BodyColor.a;
                }

                // 叠加纹身贴图。单色线稿照旧"贴图色 x 墨水色"(与游戏内一致);
                // 但贴图自带彩色、墨色又还是默认黑时, 直接呈现贴图原色, 避免彩绘/口红等被乘黑。
                if (Params.x > 0.5f) {
                    float4 tex = TattooTexture.Sample(SamplerLinear, input.UV);
                    float tatAlpha = tex.a * TattooColor.a;
                    if (tatAlpha > 0.005f) {
                        float3 ink = TattooColor.rgb;
                        float  inkMax = max(ink.r, max(ink.g, ink.b));
                        float  texChroma = max(tex.r, max(tex.g, tex.b)) - min(tex.r, min(tex.g, tex.b));

                        float3 tatRgb = (texChroma > 0.08f && inkMax < 0.05f) ? tex.rgb : (tex.rgb * ink);

                        // 结合立体漫反射与明暗体积, 纯正饱和度直接混合, 杜绝亮度污染产生的暗红/浑浊感
                        float3 tatLit = tatRgb * (shadeBase + 0.20f * shadeW * volume);
                        color = lerp(color, tatLit, tatAlpha);

                        // 全息模式让纹身自身具备不透明度支撑
                        if (Params.z < 0.5f) {
                            alpha = max(alpha, tatAlpha * 0.95f);
                        }
                    }

                    // 自发光与皮肤上的辐射泛光 / 散光 (Emissive Bloom & Halo spreading onto skin)
                    if (TattooGlow.a > 0.5f) {
                        // 1. 纹身核心高亮自发光
                        float coreGlow = tex.a * TattooColor.a;

                        // 2. 利用 Mipmap 高斯模糊捕获大范围辐射光晕
                        float nearBloom = TattooTexture.SampleLevel(SamplerLinear, input.UV, 2.5f).a;
                        float wideBloom = TattooTexture.SampleLevel(SamplerLinear, input.UV, 4.5f).a;

                        // 3. 8 方向径向采样，确保无 mipmap 贴图也能产生向外散溢到皮肤的柔和光晕
                        float tapGlow = 0.0f;
                        static const float2 kGlowOffsets[8] = {
                            float2( 0.0035f,  0.0000f),
                            float2(-0.0035f,  0.0000f),
                            float2( 0.0000f,  0.0035f),
                            float2( 0.0000f, -0.0035f),
                            float2( 0.0025f,  0.0025f),
                            float2(-0.0025f,  0.0025f),
                            float2( 0.0025f, -0.0025f),
                            float2(-0.0025f, -0.0025f)
                        };
                        [unroll]
                        for (int k = 0; k < 8; ++k) {
                            tapGlow += TattooTexture.SampleLevel(SamplerLinear, input.UV + kGlowOffsets[k], 0).a;
                        }
                        tapGlow *= 0.125f;

                        // 漫射扩散至周围皮肤的散光光晕 (Halo onto surrounding skin)
                        float skinHalo = max(tapGlow * 0.70f, max(nearBloom * 0.75f, wideBloom * 0.40f)) * TattooColor.a;

                        // 合成总发光：核心强光 (1.5x) + 皮肤扩散泛光 (1.35x)
                        float totalGlow = coreGlow * 1.5f + skinHalo * 1.35f;

                        if (totalGlow > 0.003f) {
                            color += TattooGlow.rgb * totalGlow;
                            if (Params.z < 0.5f) {
                                alpha = max(alpha, saturate(totalGlow * 0.85f));
                            }
                        }
                    }
                }

                return float4(color, alpha);
            }
        )";

        struct ConstantBufferData
        {
            DirectX::XMMATRIX worldViewProj;
            DirectX::XMMATRIX world;
            DirectX::XMFLOAT4 bodyColor;
            DirectX::XMFLOAT4 tattooColor;
            DirectX::XMFLOAT4 cameraPos;
            DirectX::XMFLOAT4 params;
            DirectX::XMFLOAT4 tattooGlow;
        };

        // 生成优雅的 3D 人偶身体网格（标准老滚 Body UV 躯干、腰肢、臀腿基础轮廓）
        void GenerateBodyMesh(std::vector<MannequinVertex>& a_vertices, std::vector<std::uint32_t>& a_indices)
        {
            a_vertices.clear();
            a_indices.clear();

            constexpr int slices = 36;
            constexpr int stacks = 40;

            // 高度从 Y = -1.1 到 Y = 1.1
            for (int i = 0; i <= stacks; ++i) {
                float v = static_cast<float>(i) / static_cast<float>(stacks);
                float y = (v - 0.5f) * 2.2f;

                // 拟合女性人体截面半径（胸围、细腰、骨盆、大腿轮廓曲线）
                float r = 0.30f;
                if (y > 0.4f) {
                    // 胸腔与胸部
                    float t = (y - 0.4f) / 0.5f;
                    r = 0.34f + 0.08f * std::sin(t * 3.14159f);
                } else if (y > -0.1f) {
                    // 细腰部收束
                    float t = (y + 0.1f) / 0.5f;
                    r = 0.25f + 0.06f * (1.0f - std::sin(t * 3.14159f));
                } else {
                    // 骨盆、臀部与大腿
                    float t = (-y - 0.1f) / 1.0f;
                    r = 0.35f + 0.05f * std::cos(t * 2.5f);
                }

                for (int j = 0; j <= slices; ++j) {
                    float u = static_cast<float>(j) / static_cast<float>(slices);
                    // 对齐老滚标准人体 UV：u = 0.5 为正面中央（前胸/腹部），u = 0 与 u = 1 为后背脊椎
                    float theta = (u - 0.5f) * 2.0f * 3.14159265f;

                    // 椭圆截面（前后稍扁，符合人体横截面）
                    float rx = r * 1.15f;
                    float rz = r * 0.85f;

                    // 胸部与背部细化微调 (theta = 0 为正前方 +Z)
                    if (y > 0.3f && y < 0.7f) {
                        float frontFactor = std::max(0.0f, std::cos(theta));
                        rz += frontFactor * 0.07f; // 前胸凸起
                    }

                    float x = rx * std::sin(theta);
                    float z = rz * std::cos(theta);

                    MannequinVertex vert{};
                    vert.pos = { x, y, z };

                    // 计算法线
                    DirectX::XMFLOAT3 n{ x / rx, 0.1f, z / rz };
                    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    vert.norm = { n.x / len, n.y / len, n.z / len };

                    // UV 空间：u 对齐老滚水平环绕 (0.5 为前胸)，v 从头到脚对应贴图垂直方向
                    vert.uv = { u, 1.0f - v };

                    a_vertices.push_back(vert);
                }
            }

            for (int i = 0; i < stacks; ++i) {
                for (int j = 0; j < slices; ++j) {
                    int first = i * (slices + 1) + j;
                    int second = first + slices + 1;

                    a_indices.push_back(first);
                    a_indices.push_back(second);
                    a_indices.push_back(first + 1);

                    a_indices.push_back(second);
                    a_indices.push_back(second + 1);
                    a_indices.push_back(first + 1);
                }
            }
        }

        // 生成 3D 人体头部网格（老滚 Face UV 规范：u = 0.5 为面部中央鼻梁/前额，u = 0 与 u = 1 为后脑耳后）
        void GenerateHeadMesh(std::vector<MannequinVertex>& a_vertices, std::vector<std::uint32_t>& a_indices)
        {
            a_vertices.clear();
            a_indices.clear();

            constexpr int slices = 36;
            constexpr int stacks = 32;

            for (int i = 0; i <= stacks; ++i) {
                float v = static_cast<float>(i) / static_cast<float>(stacks);
                // 老滚 Face UV: v = 0.0 为头顶，v = 1.0 为颈部下方
                float y = (0.5f - v) * 1.5f;

                float r = 0.38f;
                if (y > 0.15f) {
                    float t = std::clamp((y - 0.15f) / 0.60f, 0.0f, 1.0f);
                    r = 0.38f * std::sqrt(std::max(0.01f, 1.0f - t * t));
                } else if (y < -0.25f) {
                    float t = std::clamp((-y - 0.25f) / 0.50f, 0.0f, 1.0f);
                    r = 0.38f - 0.14f * t;
                }

                for (int j = 0; j <= slices; ++j) {
                    float u = static_cast<float>(j) / static_cast<float>(slices);
                    float theta = (u - 0.5f) * 2.0f * 3.14159265f;

                    float rx = r * 0.92f;
                    float rz = r * 1.05f;

                    float front = std::max(0.0f, std::cos(theta));
                    if (y > -0.2f && y < 0.25f) {
                        rz += front * 0.06f; // 面颊
                    }
                    if (y > -0.05f && y < 0.12f && front > 0.7f) {
                        rz += (front - 0.7f) * 0.15f; // 鼻梁
                    }

                    float x = rx * std::sin(theta);
                    float z = rz * std::cos(theta);

                    MannequinVertex vert{};
                    vert.pos = { x, y, z };

                    DirectX::XMFLOAT3 n{ x / std::max(0.01f, rx), (y * 0.3f), z / std::max(0.01f, rz) };
                    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    vert.norm = (len > 0.001f) ? DirectX::XMFLOAT3{ n.x / len, n.y / len, n.z / len } : DirectX::XMFLOAT3{ 0.f, 0.f, 1.f };

                    vert.uv = { u, v };
                    a_vertices.push_back(vert);
                }
            }

            for (int i = 0; i < stacks; ++i) {
                for (int j = 0; j < slices; ++j) {
                    int first = i * (slices + 1) + j;
                    int second = first + slices + 1;

                    a_indices.push_back(first);
                    a_indices.push_back(first + 1);
                    a_indices.push_back(second);

                    a_indices.push_back(second);
                    a_indices.push_back(first + 1);
                    a_indices.push_back(second + 1);
                }
            }

            NifMeshExtractor::ComputeSmoothNormals(a_vertices, a_indices);
        }

        // 生成 3D 手部网格（老滚 Hands UV 规范：左手在 u in [0, 0.5]，右手在 u in [0.5, 1.0]）
        void GenerateHandsMesh(std::vector<MannequinVertex>& a_vertices, std::vector<std::uint32_t>& a_indices)
        {
            a_vertices.clear();
            a_indices.clear();

            constexpr int slices = 20;
            constexpr int stacks = 24;

            for (int side = 0; side < 2; ++side) {
                const float centerX = (side == 0) ? -0.42f : 0.42f;
                const float uMin = (side == 0) ? 0.0f : 0.5f;
                const float uMax = (side == 0) ? 0.5f : 1.0f;
                const std::uint32_t baseVertex = static_cast<std::uint32_t>(a_vertices.size());

                for (int i = 0; i <= stacks; ++i) {
                    float v = static_cast<float>(i) / static_cast<float>(stacks);
                    float y = (0.5f - v) * 1.4f;

                    float rx = 0.18f;
                    float rz = 0.08f;
                    if (y > 0.2f) {
                        rx = 0.14f;
                        rz = 0.11f;
                    } else if (y < -0.3f) {
                        float t = (-y - 0.3f) / 0.4f;
                        rx = 0.18f - 0.06f * t;
                        rz = 0.08f - 0.03f * t;
                    }

                    for (int j = 0; j <= slices; ++j) {
                        float segU = static_cast<float>(j) / static_cast<float>(slices);
                        float u = uMin + segU * (uMax - uMin);
                        float theta = segU * 2.0f * 3.14159265f;

                        float x = centerX + rx * std::sin(theta);
                        float z = rz * std::cos(theta);

                        MannequinVertex vert{};
                        vert.pos = { x, y, z };

                        DirectX::XMFLOAT3 n{ std::sin(theta), 0.1f, std::cos(theta) };
                        float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        vert.norm = { n.x / len, n.y / len, n.z / len };

                        vert.uv = { u, v };
                        a_vertices.push_back(vert);
                    }
                }

                for (int i = 0; i < stacks; ++i) {
                    for (int j = 0; j < slices; ++j) {
                        std::uint32_t first = baseVertex + i * (slices + 1) + j;
                        std::uint32_t second = first + slices + 1;

                        a_indices.push_back(first);
                        a_indices.push_back(second);
                        a_indices.push_back(first + 1);

                        a_indices.push_back(second);
                        a_indices.push_back(second + 1);
                        a_indices.push_back(first + 1);
                    }
                }
            }
        }

        // 生成 3D 脚部网格（老滚 Feet UV 规范：左脚在 u in [0, 0.5]，右脚在 u in [0.5, 1.0]）
        void GenerateFeetMesh(std::vector<MannequinVertex>& a_vertices, std::vector<std::uint32_t>& a_indices)
        {
            a_vertices.clear();
            a_indices.clear();

            constexpr int slices = 20;
            constexpr int stacks = 24;

            for (int side = 0; side < 2; ++side) {
                const float centerX = (side == 0) ? -0.35f : 0.35f;
                const float uMin = (side == 0) ? 0.0f : 0.5f;
                const float uMax = (side == 0) ? 0.5f : 1.0f;
                const std::uint32_t baseVertex = static_cast<std::uint32_t>(a_vertices.size());

                for (int i = 0; i <= stacks; ++i) {
                    float v = static_cast<float>(i) / static_cast<float>(stacks);
                    float y = (0.5f - v) * 1.3f;

                    float rx = 0.16f;
                    float rz = 0.12f;
                    float zOffset = 0.0f;
                    if (y < 0.0f) {
                        float t = -y / 0.65f;
                        zOffset = t * 0.35f;
                        rz = 0.12f + t * 0.15f;
                    }

                    for (int j = 0; j <= slices; ++j) {
                        float segU = static_cast<float>(j) / static_cast<float>(slices);
                        float u = uMin + segU * (uMax - uMin);
                        float theta = segU * 2.0f * 3.14159265f;

                        float x = centerX + rx * std::sin(theta);
                        float z = zOffset + rz * std::cos(theta);

                        MannequinVertex vert{};
                        vert.pos = { x, y, z };

                        DirectX::XMFLOAT3 n{ std::sin(theta), 0.15f, std::cos(theta) };
                        float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        vert.norm = { n.x / len, n.y / len, n.z / len };

                        vert.uv = { u, v };
                        a_vertices.push_back(vert);
                    }
                }

                for (int i = 0; i < stacks; ++i) {
                    for (int j = 0; j < slices; ++j) {
                        std::uint32_t first = baseVertex + i * (slices + 1) + j;
                        std::uint32_t second = first + slices + 1;

                        a_indices.push_back(first);
                        a_indices.push_back(second);
                        a_indices.push_back(first + 1);

                        a_indices.push_back(second);
                        a_indices.push_back(second + 1);
                        a_indices.push_back(first + 1);
                    }
                }
            }
        }
    }

    MannequinRenderer& MannequinRenderer::Get()
    {
        static MannequinRenderer s_instance;
        return s_instance;
    }

    bool MannequinRenderer::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
    {
        if (_initialized) {
            return true;
        }

        if (!a_device || !a_context) {
            return false;
        }

        _initFailed = false;
        _device = a_device;
        _device->AddRef();
        _context = a_context;
        _context->AddRef();

        for (std::size_t i = 0; i < static_cast<std::size_t>(PreviewPart::kCount); ++i) {
            ResetCamera(static_cast<PreviewPart>(i));
            // -1, not 0: "this slot holds nothing yet", so the first batch always
            // builds a placeholder rather than assuming a female mesh is present.
            _meshSex[i] = -1;
        }

        // InitMesh() queues the NIF parse on this worker, so it has to exist first.
        // StartMeshWorker() also clears any stop flag left by a previous Shutdown.
        StartMeshWorker();

        if (!InitShaders()) {
            SKSE::log::error("[RENDER] Initialize failed at InitShaders");
            Shutdown();
            _initFailed = true;
            return false;
        }
        if (!InitMesh()) {
            SKSE::log::error("[RENDER] Initialize failed at InitMesh");
            Shutdown();
            _initFailed = true;
            return false;
        }
        if (!InitBuffers()) {
            SKSE::log::error("[RENDER] Initialize failed at InitBuffers");
            Shutdown();
            _initFailed = true;
            return false;
        }
        if (!Resize(_width, _height)) {
            SKSE::log::error("[RENDER] Initialize failed at Resize");
            Shutdown();
            _initFailed = true;
            return false;
        }

        _initialized = true;
        _initFailed = false;
        SKSE::log::info("[RENDER] MannequinRenderer initialized successfully ({}x{})", _width, _height);
        return true;
    }

    void MannequinRenderer::Shutdown()
    {
        // Before anything else: a worker mid-parse must not be left writing into
        // this process after the DLL is unloaded.
        StopMeshWorker();

        // Release all cached tattoo textures while D3D device is still alive
        TextureLoader::ClearCache();

        if (_constantBuffer) { _constantBuffer->Release(); _constantBuffer = nullptr; }
        if (_inputLayout) { _inputLayout->Release(); _inputLayout = nullptr; }
        if (_pixelShader) { _pixelShader->Release(); _pixelShader = nullptr; }
        if (_vertexShader) { _vertexShader->Release(); _vertexShader = nullptr; }

        for (auto& pm : _partMeshes) {
            pm.Release();
        }
        for (auto& sex : _meshSex) {
            sex = -1;
        }

        if (_samplerState) { _samplerState->Release(); _samplerState = nullptr; }
        if (_rasterState) { _rasterState->Release(); _rasterState = nullptr; }
        if (_blendStateDepthOnly) { _blendStateDepthOnly->Release(); _blendStateDepthOnly = nullptr; }
        if (_blendState) { _blendState->Release(); _blendState = nullptr; }
        if (_depthStencilStateTestOnly) { _depthStencilStateTestOnly->Release(); _depthStencilStateTestOnly = nullptr; }
        if (_depthStencilState) { _depthStencilState->Release(); _depthStencilState = nullptr; }

        if (_depthStencilView) { _depthStencilView->Release(); _depthStencilView = nullptr; }
        if (_depthStencilTexture) { _depthStencilTexture->Release(); _depthStencilTexture = nullptr; }
        if (_shaderResourceView) { _shaderResourceView->Release(); _shaderResourceView = nullptr; }
        if (_renderTargetView) { _renderTargetView->Release(); _renderTargetView = nullptr; }
        if (_renderTargetTexture) { _renderTargetTexture->Release(); _renderTargetTexture = nullptr; }

        if (_context) { _context->Release(); _context = nullptr; }
        if (_device) { _device->Release(); _device = nullptr; }

        _initialized = false;
        _lastFailedWidth = 0;
        _lastFailedHeight = 0;
    }

    bool MannequinRenderer::InitShaders()
    {
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

        HRESULT hr = D3DCompile(g_shaderSource, sizeof(g_shaderSource), nullptr, nullptr, nullptr,
            "VS_Main", "vs_5_0", flags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                SKSE::log::error("[RENDER] Vertex shader compilation failed: {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
                errorBlob->Release();
                errorBlob = nullptr;
            } else {
                SKSE::log::error("[RENDER] Vertex shader compilation failed (hr=0x{:X})", static_cast<unsigned long>(hr));
            }
            return false;
        }
        if (errorBlob) {
            errorBlob->Release();
            errorBlob = nullptr;
        }

        hr = D3DCompile(g_shaderSource, sizeof(g_shaderSource), nullptr, nullptr, nullptr,
            "PS_Main", "ps_5_0", flags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                SKSE::log::error("[RENDER] Pixel shader compilation failed: {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
                errorBlob->Release();
                errorBlob = nullptr;
            } else {
                SKSE::log::error("[RENDER] Pixel shader compilation failed (hr=0x{:X})", static_cast<unsigned long>(hr));
            }
            vsBlob->Release();
            return false;
        }
        if (errorBlob) {
            errorBlob->Release();
            errorBlob = nullptr;
        }

        hr = _device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_vertexShader);
        if (FAILED(hr)) {
            SKSE::log::error("[RENDER] CreateVertexShader failed (hr=0x{:X})", static_cast<unsigned long>(hr));
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        hr = _device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_pixelShader);
        if (FAILED(hr)) {
            SKSE::log::error("[RENDER] CreatePixelShader failed (hr=0x{:X})", static_cast<unsigned long>(hr));
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MannequinVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MannequinVertex, norm), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(MannequinVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        hr = _device->CreateInputLayout(layoutDesc, ARRAYSIZE(layoutDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &_inputLayout);
        vsBlob->Release();
        psBlob->Release();

        if (FAILED(hr)) {
            SKSE::log::error("[RENDER] CreateInputLayout failed (hr=0x{:X})", static_cast<unsigned long>(hr));
            return false;
        }

        SKSE::log::info("[RENDER] Shaders compiled and initialized successfully");
        return true;
    }

    bool MannequinRenderer::InitMesh()
    {
        return InitMeshWithSex(0); // 默认女性
    }

    bool MannequinRenderer::ReloadForPlayerSex()
    {
        return InitMeshWithSex(-1);
    }

    static bool CreateD3DBuffers(
        ID3D11Device* a_device,
        const std::vector<MannequinVertex>& a_vertices,
        const std::vector<std::uint32_t>& a_indices,
        ID3D11Buffer*& a_outVB,
        ID3D11Buffer*& a_outIB)
    {
        if (!a_device || a_vertices.empty() || a_indices.empty()) return false;

        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.ByteWidth = static_cast<UINT>(sizeof(MannequinVertex) * a_vertices.size());
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vData{};
        vData.pSysMem = a_vertices.data();
        if (FAILED(a_device->CreateBuffer(&vbd, &vData, &a_outVB))) return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.ByteWidth = static_cast<UINT>(sizeof(std::uint32_t) * a_indices.size());
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA iData{};
        iData.pSysMem = a_indices.data();
        if (FAILED(a_device->CreateBuffer(&ibd, &iData, &a_outIB))) {
            a_outVB->Release();
            a_outVB = nullptr;
            return false;
        }

        return true;
    }

    bool MannequinRenderer::InitMeshWithSex(int a_sex)
    {
        if (!_device) return false;

        BodySex targetSex = BodySex::kFemale;
        if (a_sex == 1) {
            targetSex = BodySex::kMale;
        } else if (a_sex == 0) {
            targetSex = BodySex::kFemale;
        } else {
            // 自动判断性别：若无法获取有效角色或仍处于主菜单无玩家状态，默认女性
            targetSex = BodySex::kFemale;
            auto* target = data::TargetState::Get().Resolve();
            if (target && target->GetActorBase()) {
                if (target->GetActorBase()->GetSex() == RE::SEXES::kMale) {
                    targetSex = BodySex::kMale;
                }
            } else {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && player->GetActorBase() && player->Is3DLoaded()) {
                    if (player->GetActorBase()->GetSex() == RE::SEXES::kMale) {
                        targetSex = BodySex::kMale;
                    }
                }
            }
        }

        _currentSex = (targetSex == BodySex::kMale) ? 1 : 0;

        // Procedural placeholders go up immediately, so the preview is never an
        // empty box while the NIFs are being parsed. A part that already has a
        // mesh keeps it - on a sex switch the old body stays until the new one
        // is ready, which reads better than flashing a mannequin blob.
        bool anySuccess = false;
        for (std::size_t i = 0; i < kPartCount; ++i) {
            if (_partMeshes[i].vertexBuffer && _partMeshes[i].indexBuffer && _partMeshes[i].indexCount > 0) {
                anySuccess = true;
                continue;
            }
            if (BuildProceduralMesh(i)) {
                anySuccess = true;
            }
        }

        if (!anySuccess) {
            SKSE::log::error("[RENDER] InitMeshWithSex failed: all mesh buffer creations failed");
            return false;
        }

        // The real geometry is parsed off-thread and uploaded by ApplyPendingMeshes.
        RequestMeshLoad(_currentSex);
        return true;
    }

    bool MannequinRenderer::BuildProceduralMesh(std::size_t a_index)
    {
        if (!_device || a_index >= kPartCount) {
            return false;
        }

        std::vector<MannequinVertex> vertices;
        std::vector<std::uint32_t>   indices;
        std::string                  sourceInfo;

        switch (static_cast<PreviewPart>(a_index)) {
        case PreviewPart::kFace:
            GenerateHeadMesh(vertices, indices);
            sourceInfo = "Head Procedural";
            break;
        case PreviewPart::kHands:
            GenerateHandsMesh(vertices, indices);
            sourceInfo = "Hands Procedural";
            break;
        case PreviewPart::kFeet:
            GenerateFeetMesh(vertices, indices);
            sourceInfo = "Feet Procedural";
            break;
        case PreviewPart::kBody:
        default:
            GenerateBodyMesh(vertices, indices);
            sourceInfo = "Body Procedural";
            break;
        }

        ID3D11Buffer* newVB = nullptr;
        ID3D11Buffer* newIB = nullptr;
        if (!CreateD3DBuffers(_device, vertices, indices, newVB, newIB)) {
            SKSE::log::warn("[RENDER] CreateD3DBuffers failed for procedural part {}", a_index);
            return false;
        }

        _partMeshes[a_index].Release();
        _partMeshes[a_index].vertexBuffer = newVB;
        _partMeshes[a_index].indexBuffer = newIB;
        _partMeshes[a_index].indexCount = static_cast<std::uint32_t>(indices.size());
        _partMeshes[a_index].meshSourceInfo = std::move(sourceInfo);
        _meshSex[a_index] = _currentSex;
        return true;
    }

    bool MannequinRenderer::ApplyPendingMeshes()
    {
        MeshLoadResult result;
        {
            std::scoped_lock lock(g_meshMutex);
            if (!g_meshResult.ready) {
                return false;
            }
            result = std::move(g_meshResult);
            g_meshResult = {};
        }

        // The user may have switched sex again while this batch was being
        // uploaded; _currentSex always wins, so a stale batch is simply dropped.
        if (result.sex != _currentSex) {
            SKSE::log::info("[RENDER] dropping a stale mesh batch (loaded for sex {}, now {})", result.sex, _currentSex);
            return false;
        }

        const char* sexLabel = (_currentSex == 1) ? "Male" : "Female";
        bool        replaced = false;

        for (std::size_t i = 0; i < kPartCount; ++i) {
            auto& mesh = result.parts[i];
            if (!mesh.success || mesh.vertices.empty() || mesh.indices.empty()) {
                // No usable NIF for this part. If what is on screen was built for
                // the other sex, a procedural stand-in is the honest fallback -
                // otherwise a male preview would keep the female hands forever.
                const bool rebuilt = (_meshSex[i] != _currentSex) && BuildProceduralMesh(i);
                SKSE::log::info("[RENDER] preview part {} has no usable NIF, {} procedural mesh", i, rebuilt ? "fell back to the" : "kept the");
                continue;
            }

            std::vector<MannequinVertex> vertices = std::move(mesh.vertices);
            std::vector<std::uint32_t>   indices = std::move(mesh.indices);

            std::string sourceInfo;
            const bool isFallback = (_currentSex == 1 && mesh.sourcePath.find("female") != std::string::npos);
            if (isFallback) {
                sourceInfo = "NIF (" + mesh.shapeName + " [Female fallback])";
            } else {
                sourceInfo = std::string(sexLabel) + " NIF (" + mesh.shapeName + ")";
            }

            ID3D11Buffer* newVB = nullptr;
            ID3D11Buffer* newIB = nullptr;
            if (!CreateD3DBuffers(_device, vertices, indices, newVB, newIB)) {
                SKSE::log::warn("[RENDER] CreateD3DBuffers failed for loaded part {}", i);
                continue;
            }

            _partMeshes[i].Release();
            _partMeshes[i].vertexBuffer = newVB;
            _partMeshes[i].indexBuffer = newIB;
            _partMeshes[i].indexCount = static_cast<std::uint32_t>(indices.size());
            _partMeshes[i].meshSourceInfo = std::move(sourceInfo);
            _meshSex[i] = _currentSex;
            replaced = true;

            SKSE::log::info("[RENDER] preview part {} <- {} ({} verts, {} tris)",
                i, _partMeshes[i].meshSourceInfo, vertices.size(), indices.size() / 3);
        }

        return replaced;
    }

    bool MannequinRenderer::LoadMeshFromNif(PreviewPart a_part, const std::string& a_nifPath)
    {
        if (!_device) return false;

        auto idx = static_cast<std::size_t>(a_part);
        if (idx >= static_cast<std::size_t>(PreviewPart::kCount)) return false;

        auto nifMesh = NifMeshExtractor::LoadFromNif(a_nifPath);
        if (!nifMesh.success || nifMesh.vertices.empty()) {
            return false;
        }

        ID3D11Buffer* newVB = nullptr;
        ID3D11Buffer* newIB = nullptr;
        if (!CreateD3DBuffers(_device, nifMesh.vertices, nifMesh.indices, newVB, newIB)) {
            return false;
        }

        _partMeshes[idx].Release();
        _partMeshes[idx].vertexBuffer = newVB;
        _partMeshes[idx].indexBuffer = newIB;
        _partMeshes[idx].indexCount = static_cast<std::uint32_t>(nifMesh.indices.size());
        _partMeshes[idx].meshSourceInfo = "NIF (" + nifMesh.shapeName + ")";
        // A manual override counts as current for the active sex: otherwise a
        // later batch with no NIF for this part would replace it with procedural.
        _meshSex[idx] = _currentSex;

        return true;
    }

    bool MannequinRenderer::LoadMeshFromNif(const std::string& a_nifPath)
    {
        return LoadMeshFromNif(_currentPart, a_nifPath);
    }

    bool MannequinRenderer::InitBuffers()
    {
        // 常量缓冲区
        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.ByteWidth = sizeof(ConstantBufferData);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(_device->CreateBuffer(&cbd, nullptr, &_constantBuffer))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateBuffer for constant buffer failed");
            return false;
        }

        // 深度测试状态
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(_device->CreateDepthStencilState(&dsd, &_depthStencilState))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateDepthStencilState failed");
            return false;
        }

        // 混合状态（半透明叠加）
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(_device->CreateBlendState(&bd, &_blendState))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateBlendState failed");
            return false;
        }

        // 深度预通道用的混合状态: 关闭颜色写入, 只让深度生效
        D3D11_BLEND_DESC bdDepth{};
        bdDepth.RenderTarget[0].BlendEnable = FALSE;
        bdDepth.RenderTarget[0].RenderTargetWriteMask = 0;
        if (FAILED(_device->CreateBlendState(&bdDepth, &_blendStateDepthOnly))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateBlendState (depth prepass) failed");
            return false;
        }

        // 正式绘制用的深度状态: 深度只测试不写入, 且必须用 EQUAL。
        // 预通道已经把"每个像素最近的深度"写成最小值, 用 LESS_EQUAL 的话,
        // 3BA 那种乳房网格与躯干近共面重叠的地方, 第二层深度只要 <= 第一层就照样通过,
        // 同一个像素被混合两次(alpha 0.90 -> 0.99), 于是出现"一片片更实/更不透明"的大块
        // 不均匀 —— 这正是之前一直没修掉的现象。EQUAL 只放行与预通道深度完全相同的那一层,
        // 每个像素恰好混合一次。两个通道的顶点着色器与常量完全一致, 深度值是逐位可复现的。
        D3D11_DEPTH_STENCIL_DESC dsdTestOnly = dsd;
        dsdTestOnly.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsdTestOnly.DepthFunc = D3D11_COMPARISON_EQUAL;
        if (FAILED(_device->CreateDepthStencilState(&dsdTestOnly, &_depthStencilStateTestOnly))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateDepthStencilState (test only) failed");
            return false;
        }

        // 光栅化状态
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE; // 双面完整渲染，结合深度缓冲自动遮挡，避免顺逆时针差异导致正面被剔除
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(_device->CreateRasterizerState(&rd, &_rasterState))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateRasterizerState failed");
            return false;
        }

        // 采样器
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(_device->CreateSamplerState(&sd, &_samplerState))) {
            SKSE::log::error("[RENDER] InitBuffers: CreateSamplerState failed");
            return false;
        }

        SKSE::log::info("[RENDER] Pipeline state buffers initialized successfully");
        return true;
    }

    bool MannequinRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
    {
        if (a_width == 0 || a_height == 0 || !_device) return false;
        if (_lastFailedWidth == a_width && _lastFailedHeight == a_height) {
            return false;
        }

        if (_shaderResourceView) { _shaderResourceView->Release(); _shaderResourceView = nullptr; }
        if (_renderTargetView) { _renderTargetView->Release(); _renderTargetView = nullptr; }
        if (_renderTargetTexture) { _renderTargetTexture->Release(); _renderTargetTexture = nullptr; }

        if (_depthStencilView) { _depthStencilView->Release(); _depthStencilView = nullptr; }
        if (_depthStencilTexture) { _depthStencilTexture->Release(); _depthStencilTexture = nullptr; }

        // 创建颜色缓冲
        D3D11_TEXTURE2D_DESC td{};
        td.Width = a_width;
        td.Height = a_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(_device->CreateTexture2D(&td, nullptr, &_renderTargetTexture))) {
            SKSE::log::error("[RENDER] Resize: CreateTexture2D for color buffer failed");
            _lastFailedWidth = a_width; _lastFailedHeight = a_height;
            _width = 0; _height = 0;
            return false;
        }
        if (FAILED(_device->CreateRenderTargetView(_renderTargetTexture, nullptr, &_renderTargetView))) {
            SKSE::log::error("[RENDER] Resize: CreateRenderTargetView failed");
            _lastFailedWidth = a_width; _lastFailedHeight = a_height;
            _width = 0; _height = 0;
            return false;
        }
        if (FAILED(_device->CreateShaderResourceView(_renderTargetTexture, nullptr, &_shaderResourceView))) {
            SKSE::log::error("[RENDER] Resize: CreateShaderResourceView failed");
            _lastFailedWidth = a_width; _lastFailedHeight = a_height;
            _width = 0; _height = 0;
            return false;
        }

        // 创建深度缓冲
        D3D11_TEXTURE2D_DESC dtd{};
        dtd.Width = a_width;
        dtd.Height = a_height;
        dtd.MipLevels = 1;
        dtd.ArraySize = 1;
        dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dtd.SampleDesc.Count = 1;
        dtd.Usage = D3D11_USAGE_DEFAULT;
        dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        if (FAILED(_device->CreateTexture2D(&dtd, nullptr, &_depthStencilTexture))) {
            SKSE::log::error("[RENDER] Resize: CreateTexture2D for depth buffer failed");
            _lastFailedWidth = a_width; _lastFailedHeight = a_height;
            _width = 0; _height = 0;
            return false;
        }
        if (FAILED(_device->CreateDepthStencilView(_depthStencilTexture, nullptr, &_depthStencilView))) {
            SKSE::log::error("[RENDER] Resize: CreateDepthStencilView failed");
            _lastFailedWidth = a_width; _lastFailedHeight = a_height;
            _width = 0; _height = 0;
            return false;
        }

        _lastFailedWidth = 0;
        _lastFailedHeight = 0;
        _width = a_width;
        _height = a_height;
        return true;
    }

    void MannequinRenderer::Rotate(float a_deltaYaw, float a_deltaPitch)
    {
        auto& cam = GetCamera();
        cam.yaw += a_deltaYaw;
        cam.pitch += a_deltaPitch;

        // 限制俯仰角度防翻转
        constexpr float maxPitch = 1.35f;
        cam.pitch = std::clamp(cam.pitch, -maxPitch, maxPitch);
    }

    void MannequinRenderer::Zoom(float a_deltaDistance)
    {
        auto& cam = GetCamera();
        cam.distance -= a_deltaDistance;
        cam.distance = std::clamp(cam.distance, 0.6f, 5.0f);
    }

    void MannequinRenderer::Pan(float a_deltaY)
    {
        auto& cam = GetCamera();
        cam.targetY += a_deltaY;
        cam.targetY = std::clamp(cam.targetY, -1.0f, 1.0f);
    }

    void MannequinRenderer::ResetCamera(PreviewPart a_part)
    {
        auto idx = static_cast<std::size_t>(a_part);
        if (idx >= static_cast<std::size_t>(PreviewPart::kCount)) return;

        _cameras[idx].yaw = 0.0f;
        _cameras[idx].targetY = 0.0f;

        switch (a_part) {
        case PreviewPart::kFace:
            _cameras[idx].distance = 1.6f;
            _cameras[idx].pitch = 0.0f;
            break;
        case PreviewPart::kHands:
            _cameras[idx].distance = 1.7f;
            _cameras[idx].pitch = 0.25f;
            break;
        case PreviewPart::kFeet:
            _cameras[idx].distance = 1.7f;
            _cameras[idx].pitch = 0.35f;
            break;
        case PreviewPart::kBody:
        default:
            _cameras[idx].distance = 2.2f;
            _cameras[idx].pitch = 0.0f;
            break;
        }
    }

    void MannequinRenderer::ResetCamera()
    {
        ResetCamera(_currentPart);
    }

    void MannequinRenderer::SetFrontView()
    {
        auto& cam = GetCamera();
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
    }

    void MannequinRenderer::SetBackView()
    {
        auto& cam = GetCamera();
        cam.yaw = 3.14159265f;
        cam.pitch = 0.0f;
    }

    void MannequinRenderer::SetCurrentPart(PreviewPart a_part)
    {
        if (static_cast<std::uint32_t>(a_part) < static_cast<std::uint32_t>(PreviewPart::kCount)) {
            _currentPart = a_part;
        }
    }

    CameraState& MannequinRenderer::GetCamera()
    {
        return GetCamera(_currentPart);
    }

    CameraState& MannequinRenderer::GetCamera(PreviewPart a_part)
    {
        auto idx = static_cast<std::size_t>(a_part);
        if (idx >= static_cast<std::size_t>(PreviewPart::kCount)) idx = 0;
        return _cameras[idx];
    }

    const std::string& MannequinRenderer::GetMeshSourceInfo() const
    {
        return GetMeshSourceInfo(_currentPart);
    }

    const std::string& MannequinRenderer::GetMeshSourceInfo(PreviewPart a_part) const
    {
        auto idx = static_cast<std::size_t>(a_part);
        if (idx >= static_cast<std::size_t>(PreviewPart::kCount)) idx = 0;
        return _partMeshes[idx].meshSourceInfo;
    }

    ID3D11ShaderResourceView* MannequinRenderer::RenderFrame(
        ID3D11ShaderResourceView* a_tattooTexture,
        const float a_tattooColor[3],
        float a_tattooAlpha,
        bool a_isBackArea,
        MannequinColor a_bodyColor,
        float a_shadeStrength,
        const float a_glowColor[3])
    {
        // Upload any mesh set the worker finished since the last frame. This is
        // the only place D3D buffers are created for background-loaded geometry,
        // and it runs before the buffers are read below, so a mesh that just
        // arrived is drawn in the same frame.
        if (_initialized && _device) {
            ApplyPendingMeshes();
        }

        auto partIdx = static_cast<std::size_t>(_currentPart);
        if (partIdx >= static_cast<std::size_t>(PreviewPart::kCount)) partIdx = 0;

        auto* currentVB = _partMeshes[partIdx].vertexBuffer;
        auto* currentIB = _partMeshes[partIdx].indexBuffer;
        auto  currentIndexCount = _partMeshes[partIdx].indexCount;

        // 若当前部位网格不可用，则安全回退至 Body 网格
        if (!currentVB || !currentIB || currentIndexCount == 0) {
            partIdx = static_cast<std::size_t>(PreviewPart::kBody);
            currentVB = _partMeshes[partIdx].vertexBuffer;
            currentIB = _partMeshes[partIdx].indexBuffer;
            currentIndexCount = _partMeshes[partIdx].indexCount;
        }

        if (!_initialized || !_device || !_context || !_renderTargetView || !_depthStencilView ||
            !currentVB || !currentIB || currentIndexCount == 0) {
            return nullptr;
        }

        if (_device->GetDeviceRemovedReason() != S_OK) {
            SKSE::log::warn("[RENDER] Direct3D device was removed/reset (0x{:X}), shutting down renderer",
                static_cast<std::uint32_t>(_device->GetDeviceRemovedReason()));
            Shutdown();
            _initFailed = true;
            return nullptr;
        }

        // 备份原有渲染状态与管线绑定，确保对游戏与后续渲染零污染
        ID3D11ShaderResourceView* prevPSSRV = nullptr;
        _context->PSGetShaderResources(0, 1, &prevPSSRV);

        // 解除可能被外部引用的 SRV 绑定以防 D3D 资源冲突
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        _context->PSSetShaderResources(0, 1, nullSRV);

        // 备份原有渲染状态与管线绑定，确保对游戏与后续渲染零污染。
        // OM 阶段必须整表备份：ENB / Community Shaders / 后处理插件可能同时绑定多个
        // RTV（MRT），只存 1 个的话还原时会把其余的槽位永久解绑，游戏画面直接变黑。
        ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* prevDSV = nullptr;
        _context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

        // OMSetRenderTargets 的 ppRenderTargetViews 必须是连续数组，因此取「最后一个非空
        // 槽位的下标 + 1」作为还原时的 NumViews，中间的 nullptr 由运行时按解绑处理。
        UINT prevRTVCount = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (prevRTVs[i] != nullptr) { prevRTVCount = i + 1; }
        }

        // 视口同理，用两段式查询：先问数量，再按数量取内容。
        // 注意 D3D11 不允许 NumViewports == 0，所以 0 个视口时干脆不改动。
        UINT numViewports = 0;
        _context->RSGetViewports(&numViewports, nullptr);
        if (numViewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
            numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        }
        D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        if (numViewports > 0) {
            _context->RSGetViewports(&numViewports, prevViewports);
        }

        ID3D11RasterizerState* prevRasterState = nullptr;
        _context->RSGetState(&prevRasterState);

        ID3D11BlendState* prevBlendState = nullptr;
        FLOAT prevBlendFactor[4] = { 0 };
        UINT prevSampleMask = 0xffffffff;
        _context->OMGetBlendState(&prevBlendState, prevBlendFactor, &prevSampleMask);

        ID3D11DepthStencilState* prevDepthState = nullptr;
        UINT prevStencilRef = 0;
        _context->OMGetDepthStencilState(&prevDepthState, &prevStencilRef);

        ID3D11InputLayout* prevInputLayout = nullptr;
        _context->IAGetInputLayout(&prevInputLayout);

        D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        _context->IAGetPrimitiveTopology(&prevTopology);

        ID3D11VertexShader* prevVS = nullptr;
        _context->VSGetShader(&prevVS, nullptr, nullptr);

        ID3D11PixelShader* prevPS = nullptr;
        _context->PSGetShader(&prevPS, nullptr, nullptr);

        ID3D11SamplerState* prevSampler = nullptr;
        _context->PSGetSamplers(0, 1, &prevSampler);

        ID3D11Buffer* prevVB = nullptr;
        UINT prevStride = 0, prevOffset = 0;
        _context->IAGetVertexBuffers(0, 1, &prevVB, &prevStride, &prevOffset);

        ID3D11Buffer* prevIB = nullptr;
        DXGI_FORMAT prevIndexFormat = DXGI_FORMAT_UNKNOWN;
        UINT prevIndexOffset = 0;
        _context->IAGetIndexBuffer(&prevIB, &prevIndexFormat, &prevIndexOffset);

        ID3D11Buffer* prevVSConstant = nullptr;
        _context->VSGetConstantBuffers(0, 1, &prevVSConstant);

        ID3D11Buffer* prevPSConstant = nullptr;
        _context->PSGetConstantBuffers(0, 1, &prevPSConstant);

        // 设置视口与目标
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(_width);
        vp.Height = static_cast<float>(_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        _context->RSSetViewports(1, &vp);

        _context->OMSetRenderTargets(1, &_renderTargetView, _depthStencilView);

        // 清屏（深灰色微透背景 + 深度清除）
        const float clearColor[4] = { 0.08f, 0.09f, 0.12f, 0.85f };
        _context->ClearRenderTargetView(_renderTargetView, clearColor);
        _context->ClearDepthStencilView(_depthStencilView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        // 计算当前部位独立相机的矩阵 (Orbit Arcball Camera)
        const auto& cam = _cameras[partIdx];
        float eyeX = cam.distance * std::cos(cam.pitch) * std::sin(cam.yaw);
        float eyeY = cam.targetY + cam.distance * std::sin(cam.pitch);
        float eyeZ = cam.distance * std::cos(cam.pitch) * std::cos(cam.yaw);

        DirectX::XMVECTOR eyePos = DirectX::XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
        DirectX::XMVECTOR targetPos = DirectX::XMVectorSet(0.0f, cam.targetY, 0.0f, 1.0f);
        DirectX::XMVECTOR upDir = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eyePos, targetPos, upDir);
        float aspect = static_cast<float>(_width) / static_cast<float>(_height);
        DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(45.0f), aspect, 0.1f, 100.0f);
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

        // HLSL 的 cbuffer matrix 默认列主序读取未经转置的 row-major XMMATRIX 字节，正好等价于行向量变换
        DirectX::XMMATRIX wvp = world * view * proj;
        DirectX::XMMATRIX worldMat = world;

        // 更新常量缓冲区
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hrMap = _context->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hrMap)) {
            auto* cb = static_cast<ConstantBufferData*>(mapped.pData);
            cb->worldViewProj = wvp;
            cb->world = worldMat;
            if (a_bodyColor == MannequinColor::kFlesh) {
                cb->bodyColor = DirectX::XMFLOAT4(0.82f, 0.68f, 0.58f, 0.90f); // 自然温润肉色
                cb->params = DirectX::XMFLOAT4(a_tattooTexture != nullptr ? 1.0f : 0.0f, a_isBackArea ? 1.0f : 0.0f, 1.0f, a_shadeStrength);
            } else {
                cb->bodyColor = DirectX::XMFLOAT4(0.25f, 0.35f, 0.50f, 0.30f); // 优雅的全息透光蓝
                cb->params = DirectX::XMFLOAT4(a_tattooTexture != nullptr ? 1.0f : 0.0f, a_isBackArea ? 1.0f : 0.0f, 0.0f, a_shadeStrength);
            }
            cb->tattooColor = DirectX::XMFLOAT4(a_tattooColor[0], a_tattooColor[1], a_tattooColor[2], a_tattooAlpha);
            cb->cameraPos = DirectX::XMFLOAT4(eyeX, eyeY, eyeZ, 1.0f);
            if (a_glowColor) {
                cb->tattooGlow = DirectX::XMFLOAT4(a_glowColor[0], a_glowColor[1], a_glowColor[2], 1.0f);
            } else {
                cb->tattooGlow = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            }
            _context->Unmap(_constantBuffer, 0);
        } else {
            SKSE::log::warn("[RENDER] ConstantBuffer Map failed (0x{:X})", static_cast<std::uint32_t>(hrMap));
            if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET ||
                _device->GetDeviceRemovedReason() != S_OK) {
                if (numViewports > 0) {
                    _context->RSSetViewports(numViewports, prevViewports);
                }
                _context->OMSetRenderTargets(prevRTVCount, prevRTVCount > 0 ? prevRTVs : nullptr, prevDSV);
                _context->RSSetState(prevRasterState);
                _context->OMSetBlendState(prevBlendState, prevBlendFactor, prevSampleMask);
                _context->OMSetDepthStencilState(prevDepthState, prevStencilRef);
                _context->IASetInputLayout(prevInputLayout);
                _context->IASetPrimitiveTopology(prevTopology);
                _context->IASetVertexBuffers(0, 1, &prevVB, &prevStride, &prevOffset);
                _context->IASetIndexBuffer(prevIB, prevIndexFormat, prevIndexOffset);
                _context->VSSetShader(prevVS, nullptr, 0);
                _context->VSSetConstantBuffers(0, 1, &prevVSConstant);
                _context->PSSetShader(prevPS, nullptr, 0);
                _context->PSSetConstantBuffers(0, 1, &prevPSConstant);
                _context->PSSetSamplers(0, 1, &prevSampler);
                _context->PSSetShaderResources(0, 1, &prevPSSRV);

                if (prevPSSRV) prevPSSRV->Release();
                for (auto* rtv : prevRTVs) {
                    if (rtv) { rtv->Release(); }
                }
                if (prevDSV) prevDSV->Release();
                if (prevRasterState) prevRasterState->Release();
                if (prevBlendState) prevBlendState->Release();
                if (prevDepthState) prevDepthState->Release();
                if (prevInputLayout) prevInputLayout->Release();
                if (prevVS) prevVS->Release();
                if (prevPS) prevPS->Release();
                if (prevSampler) prevSampler->Release();
                if (prevVB) prevVB->Release();
                if (prevIB) prevIB->Release();
                if (prevVSConstant) prevVSConstant->Release();
                if (prevPSConstant) prevPSConstant->Release();

                Shutdown();
                _initFailed = true;
                return nullptr;
            }
        }

        // 绑定渲染管线资源
        UINT stride = sizeof(MannequinVertex);
        UINT offset = 0;
        _context->IASetInputLayout(_inputLayout);
        _context->IASetVertexBuffers(0, 1, &currentVB, &stride, &offset);
        _context->IASetIndexBuffer(currentIB, DXGI_FORMAT_R32_UINT, 0);
        _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        _context->VSSetShader(_vertexShader, nullptr, 0);
        _context->VSSetConstantBuffers(0, 1, &_constantBuffer);

        _context->PSSetShader(_pixelShader, nullptr, 0);
        _context->PSSetConstantBuffers(0, 1, &_constantBuffer);
        _context->PSSetSamplers(0, 1, &_samplerState);

        if (a_tattooTexture) {
            _context->PSSetShaderResources(0, 1, &a_tattooTexture);
        } else {
            _context->PSSetShaderResources(0, 1, nullSRV);
        }

        const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
        _context->RSSetState(_rasterState);

        // 第一遍: 深度预通道。只写深度、不写颜色, 把每个像素上最近的表面先固定下来。
        // 少了这一遍, 3BA/CBBE 这类乳房网格与躯干重叠的模型会在同一像素混合两次
        // (1-(1-a)^2), 肉色模式的胸口就会明显比四肢更不透明。
        _context->PSSetShader(nullptr, nullptr, 0);
        _context->OMSetDepthStencilState(_depthStencilState, 1);
        _context->OMSetBlendState(_blendStateDepthOnly, blendFactor, 0xffffffff);
        _context->DrawIndexed(currentIndexCount, 0, 0);

        // 第二遍: 正式着色。深度只测不写, 每个像素恰好混合一层, 全幅透明度一致。
        _context->PSSetShader(_pixelShader, nullptr, 0);
        _context->OMSetDepthStencilState(_depthStencilStateTestOnly, 1);
        _context->OMSetBlendState(_blendState, blendFactor, 0xffffffff);
        _context->DrawIndexed(currentIndexCount, 0, 0);

        // 绘制完成后解绑纹理，避免后续冲突
        _context->PSSetShaderResources(0, 1, nullSRV);

        // 还原外部状态
        if (numViewports > 0) {
            _context->RSSetViewports(numViewports, prevViewports);
        }
        _context->OMSetRenderTargets(prevRTVCount, prevRTVCount > 0 ? prevRTVs : nullptr, prevDSV);
        _context->RSSetState(prevRasterState);
        _context->OMSetBlendState(prevBlendState, prevBlendFactor, prevSampleMask);
        _context->OMSetDepthStencilState(prevDepthState, prevStencilRef);
        _context->IASetInputLayout(prevInputLayout);
        _context->IASetPrimitiveTopology(prevTopology);
        _context->IASetVertexBuffers(0, 1, &prevVB, &prevStride, &prevOffset);
        _context->IASetIndexBuffer(prevIB, prevIndexFormat, prevIndexOffset);
        _context->VSSetShader(prevVS, nullptr, 0);
        _context->VSSetConstantBuffers(0, 1, &prevVSConstant);
        _context->PSSetShader(prevPS, nullptr, 0);
        _context->PSSetConstantBuffers(0, 1, &prevPSConstant);
        _context->PSSetSamplers(0, 1, &prevSampler);
        _context->PSSetShaderResources(0, 1, &prevPSSRV);

        // 释放临时引用的 COM 对象
        if (prevPSSRV) prevPSSRV->Release();
        for (auto* rtv : prevRTVs) {
            if (rtv) { rtv->Release(); }
        }
        if (prevDSV) prevDSV->Release();
        if (prevRasterState) prevRasterState->Release();
        if (prevBlendState) prevBlendState->Release();
        if (prevDepthState) prevDepthState->Release();
        if (prevInputLayout) prevInputLayout->Release();
        if (prevVS) prevVS->Release();
        if (prevPS) prevPS->Release();
        if (prevSampler) prevSampler->Release();
        if (prevVB) prevVB->Release();
        if (prevIB) prevIB->Release();
        if (prevVSConstant) prevVSConstant->Release();
        if (prevPSConstant) prevPSConstant->Release();

        return _shaderResourceView;
    }
}
