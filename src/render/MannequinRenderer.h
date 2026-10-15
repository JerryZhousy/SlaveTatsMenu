#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <memory>
#include <string>
#include <vector>
#include "core/Config.h"

namespace stm::render
{
    struct MannequinVertex
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 norm;
        DirectX::XMFLOAT2 uv;
    };

    struct CameraState
    {
        float yaw = 0.0f;       // 水平旋转角 (radians)
        float pitch = 0.0f;     // 俯仰角 (radians)
        float distance = 2.2f;  // 距目标点距离
        float targetY = 0.0f;   // 观察目标 Y 轴偏移 (例如上下观察胸/背/腿)
    };

    class MannequinRenderer
    {
    public:
        static MannequinRenderer& Get();

        bool Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
        void Shutdown();

        // 调整离屏缓冲尺寸
        bool Resize(std::uint32_t a_width, std::uint32_t a_height);

        // 渲染单帧并返回 ImGui 可用的 ShaderResourceView 指针
        ID3D11ShaderResourceView* RenderFrame(
            ID3D11ShaderResourceView* a_tattooTexture,
            const float a_tattooColor[3],
            float a_tattooAlpha,
            bool a_isBackArea = false,
            MannequinColor a_bodyColor = MannequinColor::kTransparent
        );

        // 交互控制
        void Rotate(float a_deltaYaw, float a_deltaPitch);
        void Zoom(float a_deltaDistance);
        void Pan(float a_deltaY);
        void ResetCamera();

        // 状态读取
        CameraState& GetCamera() { return _camera; }
        bool IsInitialized() const { return _initialized; }
        std::uint32_t GetWidth() const { return _width; }
        std::uint32_t GetHeight() const { return _height; }
        const std::string& GetMeshSourceInfo() const { return _meshSourceInfo; }
        int GetCurrentSex() const { return _currentSex; }

        // 支持重载指定的 NIF 网格与根据主角性别重载
        bool LoadMeshFromNif(const std::string& a_nifPath);
        bool ReloadForPlayerSex();
        bool InitMeshWithSex(int a_sex);

    private:
        MannequinRenderer() = default;
        ~MannequinRenderer() { Shutdown(); }

        std::string _meshSourceInfo = "Procedural";
        int         _currentSex = 0;

        bool InitShaders();
        bool InitMesh();
        bool InitBuffers();

        bool _initialized = false;
        std::uint32_t _width = 384;
        std::uint32_t _height = 384;

        ID3D11Device*        _device = nullptr;
        ID3D11DeviceContext* _context = nullptr;

        // 离屏渲染目标
        ID3D11Texture2D*          _renderTargetTexture = nullptr;
        ID3D11RenderTargetView*   _renderTargetView = nullptr;
        ID3D11ShaderResourceView* _shaderResourceView = nullptr;

        // 深度测试缓冲
        ID3D11Texture2D*        _depthStencilTexture = nullptr;
        ID3D11DepthStencilView* _depthStencilView = nullptr;
        ID3D11DepthStencilState* _depthStencilState = nullptr;

        // 混合状态（半透明玻璃混合）
        ID3D11BlendState* _blendState = nullptr;
        ID3D11RasterizerState* _rasterState = nullptr;
        ID3D11SamplerState*    _samplerState = nullptr;

        // 着色器管线
        ID3D11VertexShader* _vertexShader = nullptr;
        ID3D11PixelShader*  _pixelShader = nullptr;
        ID3D11InputLayout*  _inputLayout = nullptr;
        ID3D11Buffer*       _constantBuffer = nullptr;

        // 模特网格缓冲
        ID3D11Buffer* _vertexBuffer = nullptr;
        ID3D11Buffer* _indexBuffer = nullptr;
        std::uint32_t _indexCount = 0;

        CameraState _camera;
    };
}
