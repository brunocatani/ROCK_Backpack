#pragma once
#include "PanelPoseMath.h"
#include "BackpackZones.h"
#include "ui/ItemImage.h"
#include <DirectXMath.h>
#include <filesystem>
#include <optional>
#include <vector>

namespace preview
{
    namespace math = rock_backpack::panel_math;
    struct Ray { math::Vec3 origin, direction; };
    struct Hit { float distance{}, x{}, y{}; };
    std::optional<Hit> intersect(const Ray&, const math::PanelPose&, float width, float height);
    std::optional<float> intersectBox(const Ray&, math::Vec3 center, math::Vec3 half);
    class Scene3D
    {
    public:
        bool initialize(ID3D11Device*, const std::filesystem::path& assets, unsigned width, unsigned height);
        bool resize(ID3D11Device*, unsigned width, unsigned height);
        void camera(float yaw, float pitch, float distance, bool panelOnLeft = false);
        void begin(ID3D11DeviceContext*, ID3D11RenderTargetView*);
        void backpack(ID3D11DeviceContext*);
        void weapon(ID3D11DeviceContext*, rock_backpack::storage::Location, const rock_backpack::zones::Layout&, bool selected);
        void panel(ID3D11DeviceContext*, ID3D11ShaderResourceView*, const math::PanelPose&, float width, float height, bool transparent=false);
        math::Vec3 screen(math::Vec3) const;
        Ray ray(float x, float y) const;
    private:
        struct Vertex { float p[3], normal[3], uv[2]; };
        struct Mesh {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
            unsigned count{}, texture{};
            DirectX::XMFLOAT4 color{1,1,1,1};
            float cutoff{-1};
            bool blend{};
        };
        struct Model {
            std::vector<Mesh> meshes;
            std::vector<rock_backpack::ui::ItemImage> textures;
        };
        bool loadModel(ID3D11Device*, const std::filesystem::path&, Model&, bool preparedBackpack = false);
        void drawModel(ID3D11DeviceContext*, const Model&, const DirectX::XMMATRIX&, bool selected = false);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> _layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _quad, _constants;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _overlayDepth;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _overlayBlend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
        Model _backpack;
        std::array<Model,3> _weapons;
        DirectX::XMMATRIX _viewProjection{}, _inverse{};
        unsigned _width{}, _height{};
    };
}
