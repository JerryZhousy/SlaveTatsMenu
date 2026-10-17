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

    enum class PreviewPart : std::uint32_t
    {
        kBody = 0,
        kFace = 1,
        kHands = 2,
        kFeet = 3,
        kCount = 4
    };

    struct PartMeshBuffer
    {
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        std::uint32_t indexCount = 0;
        std::string   meshSourceInfo;

        void Release()
        {
            if (vertexBuffer) { vertexBuffer->Release(); vertexBuffer = nullptr; }
            if (indexBuffer) { indexBuffer->Release(); indexBuffer = nullptr; }
            indexCount = 0;
            meshSourceInfo.clear();
        }
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
        // a_shadeStrength: 体积明暗强度 0..1。3BA 身体由许多互相重叠的附加网格拼成,
        // 各块法线朝向不同, 只要留一点明暗就会在块边界显出"一块块深浅斑", 所以默认 0(近纯色)。
        ID3D11ShaderResourceView* RenderFrame(
            ID3D11ShaderResourceView* a_tattooTexture,
            const float a_tattooColor[3],
            float a_tattooAlpha,
            bool a_isBackArea = false,
            MannequinColor a_bodyColor = MannequinColor::kFlesh,
            float a_shadeStrength = 0.0f
        );

        // 部位切换与读取
        void SetCurrentPart(PreviewPart a_part);
        PreviewPart GetCurrentPart() const { return _currentPart; }

        // 交互控制 (针对当前活跃部位的独立相机)
        void Rotate(float a_deltaYaw, float a_deltaPitch);
        void Zoom(float a_deltaDistance);
        void Pan(float a_deltaY);
        void ResetCamera();
        void ResetCamera(PreviewPart a_part);
        void SetFrontView();
        void SetBackView();

        // 状态读取
        CameraState& GetCamera();
        CameraState& GetCamera(PreviewPart a_part);
        bool IsInitialized() const { return _initialized; }
        bool IsInitFailed() const { return _initFailed; }
        void ClearInitFailed()
        {
            _initFailed = false;
            _lastFailedWidth = 0;
            _lastFailedHeight = 0;
        }
        std::uint32_t GetWidth() const { return _width; }
        std::uint32_t GetHeight() const { return _height; }
        ID3D11ShaderResourceView* GetShaderResourceView() const { return _shaderResourceView; }
        const std::string& GetMeshSourceInfo() const;
        const std::string& GetMeshSourceInfo(PreviewPart a_part) const;
        int GetCurrentSex() const { return _currentSex; }

        // 支持重载指定的 NIF 网格与根据主角性别重载
        bool LoadMeshFromNif(PreviewPart a_part, const std::string& a_nifPath);
        bool LoadMeshFromNif(const std::string& a_nifPath);
        bool ReloadForPlayerSex();
        bool InitMeshWithSex(int a_sex);

    private:
        MannequinRenderer() = default;
        ~MannequinRenderer() { Shutdown(); }

        int _currentSex = 0;
        PreviewPart _currentPart = PreviewPart::kBody;

        bool InitShaders();
        bool InitMesh();
        bool InitBuffers();

        bool _initialized = false;
        bool _initFailed = false;
        std::uint32_t _width = 384;
        std::uint32_t _height = 384;
        std::uint32_t _lastFailedWidth = 0;
        std::uint32_t _lastFailedHeight = 0;

        ID3D11Device*        _device = nullptr;
        ID3D11DeviceContext* _context = nullptr;

        // 离屏渲染目标
        ID3D11Texture2D*          _renderTargetTexture = nullptr;
        ID3D11RenderTargetView*   _renderTargetView = nullptr;
        ID3D11ShaderResourceView* _shaderResourceView = nullptr;

        // 深度测试缓冲
        ID3D11Texture2D*         _depthStencilTexture = nullptr;
        ID3D11DepthStencilView*  _depthStencilView = nullptr;
        ID3D11DepthStencilState* _depthStencilState = nullptr;
        // 深度预通道之后正式绘制用: 深度只测试、不写入, 保证每个像素只混合一层
        ID3D11DepthStencilState* _depthStencilStateTestOnly = nullptr;

        // 混合状态（半透明玻璃混合）
        ID3D11BlendState*      _blendState = nullptr;
        // 深度预通道用: 关掉颜色写入, 只让深度生效
        ID3D11BlendState*      _blendStateDepthOnly = nullptr;
        ID3D11RasterizerState* _rasterState = nullptr;
        ID3D11SamplerState*    _samplerState = nullptr;

        // 着色器管线
        ID3D11VertexShader* _vertexShader = nullptr;
        ID3D11PixelShader*  _pixelShader = nullptr;
        ID3D11InputLayout*  _inputLayout = nullptr;
        ID3D11Buffer*       _constantBuffer = nullptr;

        // 4个部位的网格缓冲与独立相机状态 (Body, Face, Hands, Feet)
        PartMeshBuffer _partMeshes[static_cast<std::size_t>(PreviewPart::kCount)];
        CameraState    _cameras[static_cast<std::size_t>(PreviewPart::kCount)];
    };
}
