#include "ui/ItemImage.h"
#include "ui/PreviewAlphaPass.h"
#include <Windows.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <cstring>

namespace
{
    using Microsoft::WRL::ComPtr;
    namespace ui = rock_backpack::ui;
    void require(bool value, const char* message)
    { if (!value) { std::cerr << message << '\n'; std::exit(1); } }

    ui::ImagePixels read(ID3D11DeviceContext* context, const ui::ItemImage& image)
    {
        ui::ImageReadback readback;
        require(readback.begin(context, image), "Readback allocation failed");
        context->Flush(); // Test owns this context; production never flushes it.
        ui::ImagePixels pixels;
        for (unsigned i = 0; i < 500; ++i) {
            const auto result = readback.poll(context, pixels);
            if (result == ui::ReadbackResult::Ready) return pixels;
            require(result == ui::ReadbackResult::Pending, "Readback failed");
            Sleep(1);
        }
        require(false, "Readback did not finish"); return {};
    }
    ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, const void* pixels, unsigned stride)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 2; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA data{ pixels, stride, 0 };
        ComPtr<ID3D11Texture2D> value;
        require(SUCCEEDED(device->CreateTexture2D(&desc, &data, &value)), "Source texture failed");
        return value;
    }
}
int main(int argc, char** argv)
{
    require(argc == 2, "Expected temporary PNG path");
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    require(SUCCEEDED(com), "COM initialization failed");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device failed");
    const std::array<std::uint8_t, 16> original{ 255,0,0,255, 0,255,0,128, 0,0,255,0, 255,255,255,255 };
    auto source = texture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original.data(), 8);
    ui::ItemImage image;
    require(image.copy(context.Get(), source.Get()), "Capture failed");
    const std::array<std::uint8_t, 16> repaint{};
    context->UpdateSubresource(source.Get(), 0, nullptr, repaint.data(), 8, 0);
    source.Reset();
    const auto saved = read(context.Get(), image);
    require(saved.bytes.size() == original.size() && std::memcmp(saved.bytes.data(), original.data(), original.size()) == 0,
        "Owned image changed when pooled source was overwritten/released");
    const auto coverage = ui::inspectItemImage(saved);
    require(coverage.valid && coverage.colorPixels == 4 && coverage.alphaPixels == 3 && coverage.visiblePixels == 3,
        "Capture inspection must distinguish RGB from visible alpha coverage");
    ui::ImagePixels blank{ .width = 2, .height = 2, .stride = 8, .format = DXGI_FORMAT_R8G8B8A8_UNORM, .bytes = std::vector<std::uint8_t>(16) };
    require(ui::inspectItemImage(blank).valid && !ui::inspectItemImage(blank).alphaPixels, "Transparent capture must not count as rendered content");
    require(ui::saveItemImage(blank, argv[1]), "Blank fixture write failed");
    ui::ItemImage empty;
    require(!empty.load(device.Get(), argv[1]), "Desktop must reject the empty PNG produced by the failed native capture");
    require(image.belongsTo(device.Get()), "Lost device identity");
    ui::ImagePixels padded{64, 64, 256, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<std::uint8_t>(64 * 256)};
    for (unsigned y = 20; y < 44; ++y) for (unsigned x = 29; x < 35; ++x) {
        auto* pixel = padded.bytes.data() + y * padded.stride + x * 4; pixel[0] = pixel[3] = 255;
    }
    const auto thumbnail = ui::itemThumbnail(padded, 16);
    require(thumbnail.height == 16 && thumbnail.width < 16 && ui::inspectItemImage(thumbnail).visiblePixels,
        "Thumbnail must crop unused target space, preserve aspect and stay bounded");
    ui::ItemImage thumb;
    require(thumb.upload(device.Get(), thumbnail) && thumb.belongsTo(device.Get()), "Owned thumbnail upload failed");
    require(ui::itemThumbnail(blank).bytes.empty() && ui::itemThumbnail(padded, 0).bytes.empty(), "Empty or invalid thumbnails accepted");
    require(ui::saveItemImage(saved, argv[1]), "PNG export failed");
    ui::ItemImage loaded;
    require(loaded.load(device.Get(), argv[1]), "PNG import failed");
    require(read(context.Get(), loaded).bytes == saved.bytes, "PNG round trip changed channels or alpha");

    ComPtr<ID3D11Device> other; ComPtr<ID3D11DeviceContext> otherContext;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &other, nullptr, &otherContext)), "Second device failed");
    auto foreign = texture(other.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original.data(), 8);
    auto* old = image.view.Get();
    require(!image.copy(context.Get(), foreign.Get()) && image.view.Get() == old && !image.belongsTo(other.Get()),
        "Device mismatch accepted or replaced a valid image");

    std::array<std::uint16_t, 16> half{};
    for (unsigned i = 0; i < 4; ++i) { half[i*4] = 0x3C00; half[i*4+1] = 0x3800; half[i*4+3] = 0x3400; }
    source = texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, half.data(), 16);
    require(image.copy(context.Get(), source.Get()), "HDR capture failed");
    require(ui::saveItemImage(read(context.Get(), image), argv[1]), "HDR export failed");
    require(loaded.load(device.Get(), argv[1]), "HDR PNG import failed");
    const auto hdr = read(context.Get(), loaded);
    require(hdr.bytes[0] == 255 && hdr.bytes[1] == 128 && hdr.bytes[2] == 0 && hdr.bytes[3] == 64,
        "Half float conversion or alpha incorrect");

    // Native forward targets may have packed HDR RGB and no alpha channel.
    const std::array<std::uint32_t, 4> packed{(15u << 6) | (14u << 17) | (13u << 27), 0, 31u << 6, 15u << 6};
    source = texture(device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, packed.data(), 8);
    require(image.copy(context.Get(), source.Get()), "RGB-only forward capture failed");
    const auto rgb = read(context.Get(), image);
    const auto rgbCoverage = ui::inspectItemImage(rgb);
    require(rgbCoverage.valid && rgbCoverage.colorPixels == 2 && rgbCoverage.alphaPixels == 4,
        "Packed HDR coverage must distinguish finite RGB content from implicit opaque alpha");
    require(ui::saveItemImage(rgb, argv[1]) && loaded.load(device.Get(), argv[1]), "RGB-only PNG round trip failed");
    const auto rgbPng = read(context.Get(), loaded);
    require(rgbPng.bytes[0] == 255 && rgbPng.bytes[1] == 128 && rgbPng.bytes[2] == 64 && rgbPng.bytes[3] == 255 &&
        rgbPng.bytes[4] == 0 && rgbPng.bytes[7] == 255 && rgbPng.bytes[8] == 0, "Packed HDR channels or absent-alpha handling incorrect");

    source = texture(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, original.data(), 8);
    require(image.copy(context.Get(), source.Get()), "BGRA capture failed");
    require(ui::saveItemImage(read(context.Get(), image), argv[1]) && loaded.load(device.Get(), argv[1]), "BGRA PNG round trip failed");
    const auto bgra = read(context.Get(), loaded);
    require(bgra.bytes[0] == 0 && bgra.bytes[2] == 255 && bgra.bytes[7] == 128, "BGRA swizzle incorrect");

    D3D11_TEXTURE2D_DESC ms{};
    ms.Width = ms.Height = 2; ms.MipLevels = ms.ArraySize = 1; ms.SampleDesc.Count = 4;
    ms.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ms.BindFlags = D3D11_BIND_RENDER_TARGET;
    source.Reset();
    require(SUCCEEDED(device->CreateTexture2D(&ms, nullptr, &source)), "MSAA texture failed");
    ComPtr<ID3D11RenderTargetView> target;
    require(SUCCEEDED(device->CreateRenderTargetView(source.Get(), nullptr, &target)), "MSAA target failed");
    const float red[]{ 1, 0, 0, 1 }; context->ClearRenderTargetView(target.Get(), red);
    auto* msaaTarget = target.Get(); context->OMSetRenderTargets(1, &msaaTarget, nullptr);
    require(image.copyRenderTarget(context.Get()), "Bound MSAA target resolve failed");
    const auto resolved = read(context.Get(), image);
    require(resolved.bytes[0] == 255 && resolved.bytes[1] == 0 && resolved.bytes[3] == 255, "MSAA content incorrect");

    // Exercise the production alpha shader on a native-style opaque RGB / zero
    // alpha image, and verify every binding the pass changes is restored.
    ui::PreviewAlphaPass alpha;
    require(alpha.compile(), "Preview alpha shader compilation failed");
    D3D11_TEXTURE2D_DESC colorDesc{};
    colorDesc.Width = colorDesc.Height = 2; colorDesc.MipLevels = colorDesc.ArraySize = colorDesc.SampleDesc.Count = 1;
    colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    const std::array<std::uint8_t, 16> nativeColors{255,0,0,0, 0,255,0,0, 0,0,0,0, 0,0,0,128};
    D3D11_SUBRESOURCE_DATA colors{nativeColors.data(), 8, 0};
    source.Reset(); target.Reset();
    require(SUCCEEDED(device->CreateTexture2D(&colorDesc, &colors, &source)) &&
        SUCCEEDED(device->CreateRenderTargetView(source.Get(), nullptr, &target)), "Native-style color target failed");
    auto* targetPointer = target.Get(); context->OMSetRenderTargets(1, &targetPointer, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    const D3D11_VIEWPORT previousViewport{1, 1, 1, 1, .2f, .8f}; context->RSSetViewports(1, &previousViewport);
    auto* previousTexture = loaded.view.Get(); context->PSSetShaderResources(0, 1, &previousTexture);
    const float previousFactors[]{.2f, .3f, .4f, .5f}; context->OMSetBlendState(nullptr, previousFactors, 0x12345678);
    context->OMSetDepthStencilState(nullptr, 7);
    require(alpha.apply(context.Get()), "Alpha repair failed");
    require(image.copyRenderTarget(context.Get()), "Repaired forward target capture failed");
    // Match the observed native failure: the target contains the model here,
    // then composition clears it before target release. Our owned image must
    // survive; a copy taken at the old boundary must reproduce the empty image.
    const float transparent[]{0, 0, 0, 0}; context->ClearRenderTargetView(target.Get(), transparent);
    ui::ItemImage lateCapture;
    require(lateCapture.copyRenderTarget(context.Get()), "Late target capture failed");
    const auto lateCoverage = ui::inspectItemImage(read(context.Get(), lateCapture));
    require(lateCoverage.valid && !lateCoverage.colorPixels && !lateCoverage.alphaPixels, "Late capture must reproduce the cleared native target");
    const auto repaired = read(context.Get(), image);
    require(repaired.bytes[0] == 255 && repaired.bytes[3] == 255 && repaired.bytes[5] == 255 && repaired.bytes[7] == 255 &&
        repaired.bytes[11] == 0 && repaired.bytes[15] == 128, "Alpha repair must keep RGB, preserve background and retain existing black-pixel alpha");
    D3D11_PRIMITIVE_TOPOLOGY restoredTopology;
    context->IAGetPrimitiveTopology(&restoredTopology);
    require(restoredTopology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "Alpha pass leaked topology");
    D3D11_VIEWPORT restoredViewport{}; UINT viewportCount = 1;
    context->RSGetViewports(&viewportCount, &restoredViewport);
    require(viewportCount == 1 && std::memcmp(&restoredViewport, &previousViewport, sizeof(previousViewport)) == 0, "Alpha pass leaked viewport");
    ComPtr<ID3D11ShaderResourceView> restoredTexture; context->PSGetShaderResources(0, 1, &restoredTexture);
    require(restoredTexture.Get() == previousTexture, "Alpha pass leaked the source SRV");
    ComPtr<ID3D11BlendState> restoredBlend; float factors[4]{}; UINT mask{};
    context->OMGetBlendState(&restoredBlend, factors, &mask);
    require(!restoredBlend && mask == 0x12345678 && std::memcmp(factors, previousFactors, sizeof(factors)) == 0, "Alpha pass leaked blend state");
    ComPtr<ID3D11DepthStencilState> restoredDepth; UINT stencil{};
    context->OMGetDepthStencilState(&restoredDepth, &stencil);
    require(!restoredDepth && stencil == 7, "Alpha pass leaked depth state");
    ComPtr<ID3D11VertexShader> restoredVS; ComPtr<ID3D11PixelShader> restoredPS;
    context->VSGetShader(&restoredVS, nullptr, nullptr); context->PSGetShader(&restoredPS, nullptr, nullptr);
    require(!restoredVS && !restoredPS, "Alpha pass leaked its shaders");
    ComPtr<ID3D11RenderTargetView> restoredTarget; context->OMGetRenderTargets(1, &restoredTarget, nullptr);
    require(restoredTarget.Get() == target.Get(), "Alpha pass changed native output bindings");
    context->OMSetRenderTargets(0, nullptr, nullptr);
    auto* capturedView = image.view.Get();
    require(!image.copyRenderTarget(context.Get()) && image.view.Get() == capturedView,
        "Missing forward target must fail without replacing the captured image");
    alpha.clearDevice();
    ui::ImagePixels malformed{ .width = 4097, .height = 1, .stride = 16388, .format = DXGI_FORMAT_R8G8B8A8_UNORM };
    require(!ui::saveItemImage(malformed, argv[1]), "Oversized export accepted");
    std::cout << "Forward capture survives native target clear; blank rejection, packed HDR, alpha state, PNG and MSAA tests passed.\n";
    CoUninitialize();
}
