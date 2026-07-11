#include "PreviewAlphaPass.h"
#include <array>
#include <cstring>

namespace rock_backpack::ui
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        constexpr char vertexCode[] = R"(
            float4 main(uint id : SV_VertexID) : SV_POSITION {
                float2 uv = float2((id << 1) & 2, id & 2);
                return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
            })";
        constexpr char pixelCode[] = R"(
            Texture2D source : register(t0);
            float4 main(float4 p : SV_POSITION) : SV_TARGET {
                float4 c = source.Load(int3(int2(p.xy), 0));
                float coverage = max(abs(c.r), max(abs(c.g), abs(c.b))) > 0.001 ? 1.0 : 0.0;
                return float4(c.rgb, max(c.a, coverage));
            })";

        template<class Shader> struct ShaderBinding
        {
            ComPtr<Shader> shader;
            std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> classes;
            UINT count{};
            template<class Getter> void save(Getter get)
            {
                std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> raw{};
                count = static_cast<UINT>(raw.size());
                get(shader.GetAddressOf(), raw.data(), &count);
                for (UINT i = 0; i < count; ++i) classes[i].Attach(raw[i]);
            }
            template<class Setter> void restore(Setter set)
            {
                std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> raw{};
                for (UINT i = 0; i < count; ++i) raw[i] = classes[i].Get();
                set(shader.Get(), raw.data(), count);
            }
        };
        struct SavedState
        {
            ID3D11DeviceContext* context;
            ShaderBinding<ID3D11VertexShader> vs;
            ShaderBinding<ID3D11PixelShader> ps;
            ShaderBinding<ID3D11GeometryShader> gs;
            ShaderBinding<ID3D11HullShader> hs;
            ShaderBinding<ID3D11DomainShader> ds;
            ComPtr<ID3D11InputLayout> layout;
            ComPtr<ID3D11ShaderResourceView> texture;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth;
            ComPtr<ID3D11RasterizerState> raster;
            FLOAT factors[4]{};
            UINT mask{}, stencil{}, viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports;
            explicit SavedState(ID3D11DeviceContext* value) : context(value)
            {
                vs.save([&](auto... args) { context->VSGetShader(args...); });
                ps.save([&](auto... args) { context->PSGetShader(args...); });
                gs.save([&](auto... args) { context->GSGetShader(args...); });
                hs.save([&](auto... args) { context->HSGetShader(args...); });
                ds.save([&](auto... args) { context->DSGetShader(args...); });
                context->IAGetInputLayout(&layout); context->IAGetPrimitiveTopology(&topology);
                context->PSGetShaderResources(0, 1, &texture);
                context->OMGetBlendState(&blend, factors, &mask);
                context->OMGetDepthStencilState(&depth, &stencil);
                context->RSGetState(&raster); context->RSGetViewports(&viewportCount, viewports.data());
            }
            ~SavedState()
            {
                vs.restore([&](auto... args) { context->VSSetShader(args...); });
                ps.restore([&](auto... args) { context->PSSetShader(args...); });
                gs.restore([&](auto... args) { context->GSSetShader(args...); });
                hs.restore([&](auto... args) { context->HSSetShader(args...); });
                ds.restore([&](auto... args) { context->DSSetShader(args...); });
                context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(topology);
                auto* srv = texture.Get(); context->PSSetShaderResources(0, 1, &srv);
                context->OMSetBlendState(blend.Get(), factors, mask);
                context->OMSetDepthStencilState(depth.Get(), stencil);
                context->RSSetState(raster.Get()); context->RSSetViewports(viewportCount, viewports.data());
            }
        };
    }

    bool PreviewAlphaPass::compile()
    {
        ComPtr<ID3DBlob> errors;
        return SUCCEEDED(D3DCompile(vertexCode, std::strlen(vertexCode), "BackpackPreviewAlphaVS", nullptr, nullptr,
                   "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &_vertexCode, &errors)) &&
            SUCCEEDED(D3DCompile(pixelCode, std::strlen(pixelCode), "BackpackPreviewAlphaPS", nullptr, nullptr,
                   "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &_pixelCode, &errors));
    }
    void PreviewAlphaPass::clearDevice()
    {
        _source = {}; _vertex.Reset(); _pixel.Reset(); _blend.Reset(); _depth.Reset(); _raster.Reset(); _device.Reset();
    }
    bool PreviewAlphaPass::ensureDevice(ID3D11Device* device)
    {
        if (_device.Get() == device) return true;
        clearDevice();
        if (!device || !_vertexCode || !_pixelCode) return false;
        D3D11_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        D3D11_DEPTH_STENCIL_DESC depth{};
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        if (FAILED(device->CreateVertexShader(_vertexCode->GetBufferPointer(), _vertexCode->GetBufferSize(), nullptr, &_vertex)) ||
            FAILED(device->CreatePixelShader(_pixelCode->GetBufferPointer(), _pixelCode->GetBufferSize(), nullptr, &_pixel)) ||
            FAILED(device->CreateBlendState(&blend, &_blend)) || FAILED(device->CreateDepthStencilState(&depth, &_depth)) ||
            FAILED(device->CreateRasterizerState(&raster, &_raster))) { clearDevice(); return false; }
        _device = device;
        return true;
    }
    bool PreviewAlphaPass::apply(ID3D11DeviceContext* context)
    {
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        if (!ensureDevice(device.Get())) return false;
        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (!target) return false;
        D3D11_RENDER_TARGET_VIEW_DESC view{}; target->GetDesc(&view);
        if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0) return false;
        ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        // The engine also uses an RGB-only HDR forward target. Sampling it
        // supplies opaque alpha; there is no alpha channel to repair there.
        if (desc.Format == DXGI_FORMAT_R11G11B10_FLOAT) return true;
        if (!_source.copy(context, texture.Get())) return false;
        const SavedState restore(context);
        // RTV/DSV/UAV bindings, vertex/index buffers, scissor rectangles,
        // samplers and constant buffers are never changed by this pass.
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(_vertex.Get(), nullptr, 0); context->PSSetShader(_pixel.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
        auto* srv = _source.view.Get(); context->PSSetShaderResources(0, 1, &srv);
        const float factors[4]{}; context->OMSetBlendState(_blend.Get(), factors, UINT_MAX);
        context->OMSetDepthStencilState(_depth.Get(), 0); context->RSSetState(_raster.Get());
        const D3D11_VIEWPORT viewport{ 0, 0, float(_source.width), float(_source.height), 0, 1 };
        context->RSSetViewports(1, &viewport);
        context->Draw(3, 0);
        return true;
    }
}
