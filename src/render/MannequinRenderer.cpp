#include "render/MannequinRenderer.h"
#include "render/NifMeshExtractor.h"
#include "data/TargetState.h"
#include <d3dcompiler.h>
#include <cmath>
#include <numbers>

namespace stm::render
{
    namespace
    {
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
                float3 N = normalize(input.WorldNorm);
                float3 V = normalize(CameraPos.xyz - input.WorldPos);

                // 菲涅尔边缘泛光（Fresnel Rim Light）
                float NdotV = saturate(abs(dot(N, V)));
                float fresnel = pow(1.0f - NdotV, 2.2f);

                float3 baseBody = BodyColor.rgb;
                float3 rimGlow;
                float alpha;

                if (Params.z > 0.5f) {
                    // 自然肉色皮肤质感 (暖色轻柔高光，保持扎实的人体肤色)
                    rimGlow = float3(0.95f, 0.88f, 0.80f) * fresnel * 0.35f;
                    alpha = saturate(BodyColor.a + fresnel * 0.12f);
                } else {
                    // 默认透明全息蓝假人
                    rimGlow = float3(0.45f, 0.70f, 1.0f) * fresnel * 0.75f;
                    alpha = saturate(BodyColor.a + fresnel * 0.45f);
                }

                float3 color = baseBody + rimGlow;

                // 叠加纹身贴图
                if (Params.x > 0.5f) {
                    float4 tex = TattooTexture.Sample(SamplerLinear, input.UV);
                    float tatAlpha = tex.a * TattooColor.a;
                    if (tatAlpha > 0.01f) {
                        float3 tatRgb = tex.rgb * TattooColor.rgb;
                        color = lerp(color, tatRgb, tatAlpha);
                        alpha = max(alpha, tatAlpha * 0.95f);
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
        };

        // 生成优雅的 3D 人偶网格（标准 UV 躯干、腰肢、臀腿基础轮廓）
        void GenerateMannequinMesh(std::vector<MannequinVertex>& a_vertices, std::vector<std::uint32_t>& a_indices)
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

        _device = a_device;
        _device->AddRef();
        _context = a_context;
        _context->AddRef();

        ResetCamera();

        if (!InitShaders()) return false;
        if (!InitMesh()) return false;
        if (!InitBuffers()) return false;
        if (!Resize(_width, _height)) return false;

        _initialized = true;
        return true;
    }

    void MannequinRenderer::Shutdown()
    {
        if (_constantBuffer) { _constantBuffer->Release(); _constantBuffer = nullptr; }
        if (_inputLayout) { _inputLayout->Release(); _inputLayout = nullptr; }
        if (_pixelShader) { _pixelShader->Release(); _pixelShader = nullptr; }
        if (_vertexShader) { _vertexShader->Release(); _vertexShader = nullptr; }

        if (_vertexBuffer) { _vertexBuffer->Release(); _vertexBuffer = nullptr; }
        if (_indexBuffer) { _indexBuffer->Release(); _indexBuffer = nullptr; }

        if (_samplerState) { _samplerState->Release(); _samplerState = nullptr; }
        if (_rasterState) { _rasterState->Release(); _rasterState = nullptr; }
        if (_blendState) { _blendState->Release(); _blendState = nullptr; }
        if (_depthStencilState) { _depthStencilState->Release(); _depthStencilState = nullptr; }

        if (_depthStencilView) { _depthStencilView->Release(); _depthStencilView = nullptr; }
        if (_depthStencilTexture) { _depthStencilTexture->Release(); _depthStencilTexture = nullptr; }
        if (_shaderResourceView) { _shaderResourceView->Release(); _shaderResourceView = nullptr; }
        if (_renderTargetView) { _renderTargetView->Release(); _renderTargetView = nullptr; }
        if (_renderTargetTexture) { _renderTargetTexture->Release(); _renderTargetTexture = nullptr; }

        if (_context) { _context->Release(); _context = nullptr; }
        if (_device) { _device->Release(); _device = nullptr; }

        _initialized = false;
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
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = D3DCompile(g_shaderSource, sizeof(g_shaderSource), nullptr, nullptr, nullptr,
            "PS_Main", "ps_5_0", flags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            vsBlob->Release();
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = _device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_vertexShader);
        if (FAILED(hr)) {
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        hr = _device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_pixelShader);
        if (FAILED(hr)) {
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

        return SUCCEEDED(hr);
    }

    bool MannequinRenderer::InitMesh()
    {
        return InitMeshWithSex(-1);
    }

    bool MannequinRenderer::ReloadForPlayerSex()
    {
        return InitMeshWithSex(-1);
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
            // 自动判断性别：优先使用当前选中目标(NPC或主角)
            auto* target = data::TargetState::Get().Resolve();
            if (target && target->GetActorBase()) {
                if (target->GetActorBase()->GetSex() == RE::SEXES::kMale) {
                    targetSex = BodySex::kMale;
                }
            } else {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && player->GetActorBase()) {
                    if (player->GetActorBase()->GetSex() == RE::SEXES::kMale) {
                        targetSex = BodySex::kMale;
                    }
                }
            }
        }

        _currentSex = (targetSex == BodySex::kMale) ? 1 : 0;

        std::vector<MannequinVertex> vertices;
        std::vector<std::uint32_t> indices;

        auto nifMesh = NifMeshExtractor::LoadDefaultBodyNif(targetSex);
        if (nifMesh.success && !nifMesh.vertices.empty()) {
            vertices = std::move(nifMesh.vertices);
            indices = std::move(nifMesh.indices);
            bool isFallback = (targetSex == BodySex::kMale && nifMesh.sourcePath.find("female") != std::string::npos);
            if (isFallback) {
                _meshSourceInfo = "NIF (" + nifMesh.shapeName + " [Female fallback])";
            } else {
                _meshSourceInfo = std::string(targetSex == BodySex::kMale ? "Male NIF (" : "Female NIF (") + nifMesh.shapeName + ")";
            }
        } else {
            GenerateMannequinMesh(vertices, indices);
            _meshSourceInfo = std::string(targetSex == BodySex::kMale ? "Male Procedural" : "Female Procedural");
        }

        ID3D11Buffer* newVB = nullptr;
        ID3D11Buffer* newIB = nullptr;

        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.ByteWidth = static_cast<UINT>(sizeof(MannequinVertex) * vertices.size());
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vData{};
        vData.pSysMem = vertices.data();
        if (FAILED(_device->CreateBuffer(&vbd, &vData, &newVB))) return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.ByteWidth = static_cast<UINT>(sizeof(std::uint32_t) * indices.size());
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA iData{};
        iData.pSysMem = indices.data();
        if (FAILED(_device->CreateBuffer(&ibd, &iData, &newIB))) {
            newVB->Release();
            return false;
        }

        if (_vertexBuffer) _vertexBuffer->Release();
        if (_indexBuffer) _indexBuffer->Release();

        _vertexBuffer = newVB;
        _indexBuffer = newIB;
        _indexCount = static_cast<std::uint32_t>(indices.size());

        return true;
    }

    bool MannequinRenderer::LoadMeshFromNif(const std::string& a_nifPath)
    {
        if (!_device) return false;

        auto nifMesh = NifMeshExtractor::LoadFromNif(a_nifPath);
        if (!nifMesh.success || nifMesh.vertices.empty()) {
            return false;
        }

        ID3D11Buffer* newVB = nullptr;
        ID3D11Buffer* newIB = nullptr;

        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.ByteWidth = static_cast<UINT>(sizeof(MannequinVertex) * nifMesh.vertices.size());
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vData{};
        vData.pSysMem = nifMesh.vertices.data();
        if (FAILED(_device->CreateBuffer(&vbd, &vData, &newVB))) return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.ByteWidth = static_cast<UINT>(sizeof(std::uint32_t) * nifMesh.indices.size());
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA iData{};
        iData.pSysMem = nifMesh.indices.data();
        if (FAILED(_device->CreateBuffer(&ibd, &iData, &newIB))) {
            newVB->Release();
            return false;
        }

        if (_vertexBuffer) _vertexBuffer->Release();
        if (_indexBuffer) _indexBuffer->Release();

        _vertexBuffer = newVB;
        _indexBuffer = newIB;
        _indexCount = static_cast<std::uint32_t>(nifMesh.indices.size());
        _meshSourceInfo = "NIF (" + nifMesh.shapeName + ")";

        return true;
    }

    bool MannequinRenderer::InitBuffers()
    {
        // 常量缓冲区
        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.ByteWidth = sizeof(ConstantBufferData);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(_device->CreateBuffer(&cbd, nullptr, &_constantBuffer))) return false;

        // 深度测试状态
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(_device->CreateDepthStencilState(&dsd, &_depthStencilState))) return false;

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
        if (FAILED(_device->CreateBlendState(&bd, &_blendState))) return false;

        // 光栅化状态
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK; // 背面剔除，确保外表面与纹身清晰无内部遮挡
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(_device->CreateRasterizerState(&rd, &_rasterState))) return false;

        // 采样器
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(_device->CreateSamplerState(&sd, &_samplerState))) return false;

        return true;
    }

    bool MannequinRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
    {
        if (a_width == 0 || a_height == 0 || !_device) return false;

        _width = a_width;
        _height = a_height;

        if (_shaderResourceView) { _shaderResourceView->Release(); _shaderResourceView = nullptr; }
        if (_renderTargetView) { _renderTargetView->Release(); _renderTargetView = nullptr; }
        if (_renderTargetTexture) { _renderTargetTexture->Release(); _renderTargetTexture = nullptr; }

        if (_depthStencilView) { _depthStencilView->Release(); _depthStencilView = nullptr; }
        if (_depthStencilTexture) { _depthStencilTexture->Release(); _depthStencilTexture = nullptr; }

        // 创建颜色缓冲
        D3D11_TEXTURE2D_DESC td{};
        td.Width = _width;
        td.Height = _height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(_device->CreateTexture2D(&td, nullptr, &_renderTargetTexture))) return false;
        if (FAILED(_device->CreateRenderTargetView(_renderTargetTexture, nullptr, &_renderTargetView))) return false;
        if (FAILED(_device->CreateShaderResourceView(_renderTargetTexture, nullptr, &_shaderResourceView))) return false;

        // 创建深度缓冲
        D3D11_TEXTURE2D_DESC dtd{};
        dtd.Width = _width;
        dtd.Height = _height;
        dtd.MipLevels = 1;
        dtd.ArraySize = 1;
        dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dtd.SampleDesc.Count = 1;
        dtd.Usage = D3D11_USAGE_DEFAULT;
        dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        if (FAILED(_device->CreateTexture2D(&dtd, nullptr, &_depthStencilTexture))) return false;
        if (FAILED(_device->CreateDepthStencilView(_depthStencilTexture, nullptr, &_depthStencilView))) return false;

        return true;
    }

    void MannequinRenderer::Rotate(float a_deltaYaw, float a_deltaPitch)
    {
        _camera.yaw += a_deltaYaw;
        _camera.pitch += a_deltaPitch;

        // 限制俯仰角度防翻转
        constexpr float maxPitch = 1.35f;
        _camera.pitch = std::clamp(_camera.pitch, -maxPitch, maxPitch);
    }

    void MannequinRenderer::Zoom(float a_deltaDistance)
    {
        _camera.distance -= a_deltaDistance;
        _camera.distance = std::clamp(_camera.distance, 0.8f, 5.0f);
    }

    void MannequinRenderer::Pan(float a_deltaY)
    {
        _camera.targetY += a_deltaY;
        _camera.targetY = std::clamp(_camera.targetY, -1.0f, 1.0f);
    }

    void MannequinRenderer::ResetCamera()
    {
        _camera.yaw = 0.0f;
        _camera.pitch = 0.0f;
        _camera.distance = 2.2f;
        _camera.targetY = 0.0f;
    }

    ID3D11ShaderResourceView* MannequinRenderer::RenderFrame(
        ID3D11ShaderResourceView* a_tattooTexture,
        const float a_tattooColor[3],
        float a_tattooAlpha,
        bool a_isBackArea,
        MannequinColor a_bodyColor)
    {
        if (!_initialized || !_context || !_renderTargetView || !_depthStencilView) {
            return nullptr;
        }

        // 解除可能被外部引用的 SRV 绑定以防 D3D 资源冲突
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        _context->PSSetShaderResources(0, 1, nullSRV);

        // 备份原有渲染状态
        ID3D11RenderTargetView* prevRTV = nullptr;
        ID3D11DepthStencilView* prevDSV = nullptr;
        _context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

        D3D11_VIEWPORT prevViewport{};
        UINT numViewports = 1;
        _context->RSGetViewports(&numViewports, &prevViewport);

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

        // 计算矩阵 (Orbit Arcball Camera)
        float eyeX = _camera.distance * std::cos(_camera.pitch) * std::sin(_camera.yaw);
        float eyeY = _camera.targetY + _camera.distance * std::sin(_camera.pitch);
        float eyeZ = _camera.distance * std::cos(_camera.pitch) * std::cos(_camera.yaw);

        DirectX::XMVECTOR eyePos = DirectX::XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
        DirectX::XMVECTOR targetPos = DirectX::XMVectorSet(0.0f, _camera.targetY, 0.0f, 1.0f);
        DirectX::XMVECTOR upDir = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eyePos, targetPos, upDir);
        float aspect = static_cast<float>(_width) / static_cast<float>(_height);
        DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(45.0f), aspect, 0.1f, 100.0f);
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

        DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(world * view * proj);
        DirectX::XMMATRIX worldTrans = DirectX::XMMatrixTranspose(world);

        // 更新常量缓冲区
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(_context->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb = static_cast<ConstantBufferData*>(mapped.pData);
            cb->worldViewProj = wvp;
            cb->world = worldTrans;
            if (a_bodyColor == MannequinColor::kFlesh) {
                cb->bodyColor = DirectX::XMFLOAT4(0.82f, 0.68f, 0.58f, 0.90f); // 自然温润肉色
                cb->params = DirectX::XMFLOAT4(a_tattooTexture != nullptr ? 1.0f : 0.0f, a_isBackArea ? 1.0f : 0.0f, 1.0f, 0.0f);
            } else {
                cb->bodyColor = DirectX::XMFLOAT4(0.25f, 0.35f, 0.50f, 0.30f); // 优雅的全息透光蓝
                cb->params = DirectX::XMFLOAT4(a_tattooTexture != nullptr ? 1.0f : 0.0f, a_isBackArea ? 1.0f : 0.0f, 0.0f, 0.0f);
            }
            cb->tattooColor = DirectX::XMFLOAT4(a_tattooColor[0], a_tattooColor[1], a_tattooColor[2], a_tattooAlpha);
            cb->cameraPos = DirectX::XMFLOAT4(eyeX, eyeY, eyeZ, 1.0f);
            _context->Unmap(_constantBuffer, 0);
        }

        // 绑定渲染管线资源
        UINT stride = sizeof(MannequinVertex);
        UINT offset = 0;
        _context->IASetInputLayout(_inputLayout);
        _context->IASetVertexBuffers(0, 1, &_vertexBuffer, &stride, &offset);
        _context->IASetIndexBuffer(_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
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

        _context->OMSetDepthStencilState(_depthStencilState, 1);
        const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
        _context->OMSetBlendState(_blendState, blendFactor, 0xffffffff);
        _context->RSSetState(_rasterState);

        // 绘制人偶
        _context->DrawIndexed(_indexCount, 0, 0);

        // 绘制完成后解绑纹理，避免后续冲突
        _context->PSSetShaderResources(0, 1, nullSRV);

        // 还原外部状态
        _context->RSSetViewports(1, &prevViewport);
        _context->OMSetRenderTargets(1, &prevRTV, prevDSV);
        if (prevRTV) prevRTV->Release();
        if (prevDSV) prevDSV->Release();

        return _shaderResourceView;
    }
}
