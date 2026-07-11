#pragma once
#include "ItemImage.h"
#include <d3dcompiler.h>

namespace rock_backpack::ui
{
    // The native forward lighting pass does not provide coverage alpha. This
    // pass writes only alpha on its existing target, preserving all RGB and
    // restoring every D3D binding it changes. Compile on plugin load, not Draw.
    class PreviewAlphaPass
    {
    public:
        bool compile();
        bool apply(ID3D11DeviceContext* context);
        void clearDevice();
    private:
        bool ensureDevice(ID3D11Device* device);
        Microsoft::WRL::ComPtr<ID3DBlob> _vertexCode, _pixelCode;
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertex;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixel;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _raster;
        ItemImage _source;
    };
}
