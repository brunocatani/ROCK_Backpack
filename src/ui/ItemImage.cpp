#include "ItemImage.h"
#include <ShlObj.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rock_backpack::ui
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        DXGI_FORMAT readableFormat(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return format;
            }
        }
        unsigned pixelBytes(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
            default: return 0;
            }
        }
        float halfFloat(std::uint16_t h)
        {
            const int exponent = (h >> 10) & 31;
            const auto mantissa = h & 1023;
            const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
                exponent == 31 ? 0.f : std::ldexp(float(mantissa + 1024), exponent - 25);
            return (h & 0x8000) ? -value : value;
        }
        float unsignedFloat(std::uint32_t bits, unsigned mantissaBits)
        {
            const auto mantissa = bits & ((1u << mantissaBits) - 1);
            const auto exponent = bits >> mantissaBits;
            return exponent == 0 ? std::ldexp(float(mantissa), -14 - int(mantissaBits)) :
                exponent == 31 ? 0.f : std::ldexp(float(mantissa + (1u << mantissaBits)), int(exponent) - 15 - int(mantissaBits));
        }
        std::array<float, 3> packedColor(const std::uint8_t* p)
        {
            std::uint32_t bits; std::memcpy(&bits, p, sizeof(bits));
            return {unsignedFloat(bits & 0x7FF, 6), unsignedFloat((bits >> 11) & 0x7FF, 6), unsignedFloat(bits >> 22, 5)};
        }
    }

    bool ItemImage::belongsTo(ID3D11Device* device) const
    {
        if (!view || !device) return false;
        ComPtr<ID3D11Device> owner;
        view->GetDevice(&owner);
        return owner.Get() == device;
    }

    ImagePixels itemThumbnail(const ImagePixels& source, unsigned maximumSize)
    {
        const auto coverage = inspectItemImage(source);
        if (!coverage.valid || !coverage.visiblePixels || !maximumSize || maximumSize > 512) return {};
        ImagePixels rgba{source.width, source.height, source.width * 4, DXGI_FORMAT_R8G8B8A8_UNORM,
            std::vector<std::uint8_t>(std::size_t(source.width) * source.height * 4)};
        const auto bpp = pixelBytes(source.format);
        unsigned left = source.width, top = source.height, right{}, bottom{};
        for (unsigned y = 0; y < source.height; ++y) for (unsigned x = 0; x < source.width; ++x) {
            const auto* in = source.bytes.data() + std::size_t(y) * source.stride + x * bpp;
            auto* out = rgba.bytes.data() + (std::size_t(y) * source.width + x) * 4;
            if (source.format == DXGI_FORMAT_R11G11B10_FLOAT) {
                const auto color = packedColor(in);
                for (unsigned c = 0; c < 3; ++c) out[c] = std::uint8_t(std::clamp(color[c], 0.f, 1.f) * 255.f + .5f);
                out[3] = out[0] || out[1] || out[2] ? 255 : 0;
            } else if (bpp == 8) {
                std::uint16_t half[4]; std::memcpy(half, in, 8);
                for (unsigned c = 0; c < 4; ++c) out[c] = std::uint8_t(std::clamp(halfFloat(half[c]), 0.f, 1.f) * 255.f + .5f);
            } else if (source.format == DXGI_FORMAT_B8G8R8A8_UNORM || source.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3];
            } else std::memcpy(out, in, 4);
            if (out[3]) { left = (std::min)(left, x); top = (std::min)(top, y); right = (std::max)(right, x); bottom = (std::max)(bottom, y); }
        }
        if (left > right || top > bottom) return {};
        const auto padding = (std::max)(2u, (std::max)(right - left + 1, bottom - top + 1) / 20);
        left -= (std::min)(left, padding); top -= (std::min)(top, padding);
        right = (std::min)(source.width - 1, right + padding); bottom = (std::min)(source.height - 1, bottom + padding);
        const unsigned width = right - left + 1, height = bottom - top + 1;
        const float scale = (std::min)(1.f, float(maximumSize) / (std::max)(width, height));
        ImagePixels out;
        out.width = (std::max)(1u, unsigned(width * scale)); out.height = (std::max)(1u, unsigned(height * scale));
        out.stride = out.width * 4; out.format = rgba.format; out.bytes.resize(std::size_t(out.stride) * out.height);
        // Area filtering keeps narrow geometry visible and bounds cache memory.
        for (unsigned y = 0; y < out.height; ++y) for (unsigned x = 0; x < out.width; ++x) {
            const unsigned x0 = left + x * width / out.width, x1 = left + (x + 1) * width / out.width;
            const unsigned y0 = top + y * height / out.height, y1 = top + (y + 1) * height / out.height;
            std::uint64_t sum[4]{}; unsigned samples{};
            for (unsigned sy = y0; sy < y1; ++sy) for (unsigned sx = x0; sx < x1; ++sx) {
                const auto* pixel = rgba.bytes.data() + (std::size_t(sy) * source.width + sx) * 4;
                for (unsigned c = 0; c < 3; ++c) sum[c] += unsigned(pixel[c]) * pixel[3];
                sum[3] += pixel[3]; ++samples;
            }
            auto* pixel = out.bytes.data() + std::size_t(y) * out.stride + x * 4;
            for (unsigned c = 0; c < 3; ++c) pixel[c] = sum[3] ? std::uint8_t(sum[c] / sum[3]) : 0;
            pixel[3] = samples ? std::uint8_t(sum[3] / samples) : 0;
        }
        return out;
    }

    bool ItemImage::upload(ID3D11Device* device, const ImagePixels& pixels)
    {
        if (!device || pixels.format != DXGI_FORMAT_R8G8B8A8_UNORM || !inspectItemImage(pixels).visiblePixels) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = pixels.width; desc.Height = pixels.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = pixels.format; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.bytes.data(), pixels.stride, 0};
        ItemImage next;
        if (FAILED(device->CreateTexture2D(&desc, &data, &next.texture)) ||
            FAILED(device->CreateShaderResourceView(next.texture.Get(), nullptr, &next.view))) return false;
        next.width = pixels.width; next.height = pixels.height; *this = std::move(next); return true;
    }

    ImageCoverage inspectItemImage(const ImagePixels& pixels)
    {
        const auto bpp = pixelBytes(pixels.format);
        ImageCoverage result;
        if (!bpp || !pixels.width || !pixels.height || pixels.width > 4096 || pixels.height > 4096 ||
            pixels.stride < pixels.width * bpp || pixels.bytes.size() < std::size_t(pixels.stride) * pixels.height) return result;
        result.valid = true;
        for (unsigned y = 0; y < pixels.height; ++y) for (unsigned x = 0; x < pixels.width; ++x) {
            const auto* p = pixels.bytes.data() + std::size_t(y) * pixels.stride + x * bpp;
            bool color{}, alpha{};
            if (pixels.format == DXGI_FORMAT_R11G11B10_FLOAT) {
                const auto rgb = packedColor(p);
                color = rgb[0] > .001f || rgb[1] > .001f || rgb[2] > .001f;
                alpha = true; // RGB-only targets sample with alpha=1.
            } else if (bpp == 8) {
                std::uint16_t half[4]; std::memcpy(half, p, 8);
                color = std::abs(halfFloat(half[0])) > .001f || std::abs(halfFloat(half[1])) > .001f || std::abs(halfFloat(half[2])) > .001f;
                alpha = halfFloat(half[3]) > 0;
            } else { color = p[0] || p[1] || p[2]; alpha = p[3] != 0; }
            result.colorPixels += color; result.alphaPixels += alpha; result.visiblePixels += color && alpha;
        }
        return result;
    }

    bool ItemImage::copy(ID3D11DeviceContext* context, ID3D11Texture2D* source)
    {
        if (!context || !source || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        ComPtr<ID3D11Device> device, sourceDevice;
        context->GetDevice(&device); source->GetDevice(&sourceDevice);
        if (device != sourceDevice) return false;
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        const auto format = readableFormat(desc.Format);
        if (!desc.Width || !desc.Height || desc.Width > 4096 || desc.Height > 4096 ||
            desc.ArraySize != 1 || !pixelBytes(format)) return false;
        const auto samples = desc.SampleDesc.Count;
        desc.Format = format; desc.MipLevels = 1; desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
        ItemImage next;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &next.texture)) ||
            FAILED(device->CreateShaderResourceView(next.texture.Get(), nullptr, &next.view))) return false;
        if (samples > 1) {
            UINT support{};
            if (FAILED(device->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) return false;
            context->ResolveSubresource(next.texture.Get(), 0, source, 0, format);
        } else context->CopySubresourceRegion(next.texture.Get(), 0, 0, 0, 0, source, 0, nullptr);
        next.width = desc.Width; next.height = desc.Height;
        *this = std::move(next);
        return true;
    }

    bool ItemImage::copyRenderTarget(ID3D11DeviceContext* context)
    {
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (!target) return false;
        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{}; target->GetDesc(&targetDesc);
        if ((targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || targetDesc.Texture2D.MipSlice != 0) &&
            targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS) return false;
        ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        return SUCCEEDED(resource.As(&source)) && copy(context, source.Get());
    }

    bool ItemImage::load(ID3D11Device* device, const std::filesystem::path& path, bool thumbnail)
    {
        if (!device) return false;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converted;
        UINT w{}, h{};
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096 ||
            FAILED(factory->CreateFormatConverter(&converted)) ||
            FAILED(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        std::vector<std::uint8_t> pixels(std::size_t(w) * h * 4);
        if (FAILED(converted->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
        if (!inspectItemImage({w, h, w * 4, DXGI_FORMAT_R8G8B8A8_UNORM, pixels}).alphaPixels) return false;
        if (thumbnail) return upload(device, itemThumbnail({w, h, w * 4, DXGI_FORMAT_R8G8B8A8_UNORM, std::move(pixels)}));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ pixels.data(), w * 4, 0 };
        ItemImage next;
        if (FAILED(device->CreateTexture2D(&desc, &data, &next.texture)) ||
            FAILED(device->CreateShaderResourceView(next.texture.Get(), nullptr, &next.view))) return false;
        next.width = w; next.height = h; *this = std::move(next);
        return true;
    }

    bool ImageReadback::begin(ID3D11DeviceContext* context, const ItemImage& image)
    {
        clear();
        if (!context || !image.texture) return false;
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        if (!image.belongsTo(device.Get())) return false;
        D3D11_TEXTURE2D_DESC desc{}; image.texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &_staging))) return false;
        context->CopyResource(_staging.Get(), image.texture.Get());
        return true;
    }
    ReadbackResult ImageReadback::poll(ID3D11DeviceContext* context, ImagePixels& pixels)
    {
        if (!context || !_staging) return ReadbackResult::Failed;
        D3D11_MAPPED_SUBRESOURCE map{};
        const auto result = context->Map(_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) return ReadbackResult::Pending;
        if (FAILED(result)) { clear(); return ReadbackResult::Failed; }
        struct Unmap { ID3D11DeviceContext* c; ID3D11Texture2D* t; ~Unmap() { c->Unmap(t, 0); } } unmap{context, _staging.Get()};
        D3D11_TEXTURE2D_DESC desc{}; _staging->GetDesc(&desc);
        pixels.width = desc.Width; pixels.height = desc.Height; pixels.format = desc.Format;
        pixels.stride = desc.Width * pixelBytes(desc.Format);
        pixels.bytes.resize(std::size_t(pixels.stride) * pixels.height);
        for (unsigned y = 0; y < desc.Height; ++y)
            std::memcpy(pixels.bytes.data() + std::size_t(y) * pixels.stride,
                static_cast<const std::uint8_t*>(map.pData) + std::size_t(y) * map.RowPitch, pixels.stride);
        return ReadbackResult::Ready;
    }

    std::filesystem::path stimpakImagePath()
    {
#ifdef ROCK_BACKPACK_PREVIEW_DIRECTORY
        return std::filesystem::path(ROCK_BACKPACK_PREVIEW_DIRECTORY) / "Stimpak.png";
#else
        PWSTR documents{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) return {};
        const std::filesystem::path root(documents); CoTaskMemFree(documents);
        return root / L"My Games/Fallout4VR/Mods_Config/ROCKBackpack/ItemPreviews/Stimpak.png";
#endif
    }
    bool saveItemImage(const ImagePixels& pixels, const std::filesystem::path& path)
    {
        const auto bpp = pixelBytes(pixels.format);
        if (path.empty() || !bpp || !pixels.width || !pixels.height || pixels.width > 4096 || pixels.height > 4096 ||
            pixels.stride < pixels.width * bpp || pixels.bytes.size() < std::size_t(pixels.stride) * pixels.height) return false;
        std::vector<std::uint8_t> rgba(std::size_t(pixels.width) * pixels.height * 4);
        for (unsigned y = 0; y < pixels.height; ++y) for (unsigned x = 0; x < pixels.width; ++x) {
            const auto* in = pixels.bytes.data() + std::size_t(y) * pixels.stride + x * bpp;
            auto* out = rgba.data() + (std::size_t(y) * pixels.width + x) * 4;
            if (pixels.format == DXGI_FORMAT_R11G11B10_FLOAT) {
                const auto rgb = packedColor(in);
                for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(std::clamp(rgb[c], 0.f, 1.f) * 255.f + .5f);
                out[3] = 255;
            } else if (bpp == 8) {
                std::uint16_t half[4]; std::memcpy(half, in, 8);
                for (unsigned c = 0; c < 4; ++c) out[c] = static_cast<std::uint8_t>(std::clamp(halfFloat(half[c]), 0.f, 1.f) * 255.f + .5f);
            } else if (pixels.format == DXGI_FORMAT_B8G8R8A8_UNORM || pixels.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3];
            } else std::memcpy(out, in, 4);
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICBitmap> bitmap; ComPtr<IWICBitmapScaler> scaled; ComPtr<IWICFormatConverter> converted;
        const float scale = std::min(1.f, 512.f / std::max(pixels.width, pixels.height));
        const auto w = std::max(1u, unsigned(pixels.width * scale)), h = std::max(1u, unsigned(pixels.height * scale));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppRGBA,
                pixels.width * 4, static_cast<UINT>(rgba.size()), rgba.data(), &bitmap))) return false;
        ComPtr<IWICBitmapSource> source = bitmap;
        if (w != pixels.width || h != pixels.height) {
            if (FAILED(factory->CreateBitmapScaler(&scaled)) || FAILED(scaled->Initialize(bitmap.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
            source = scaled;
        }
        return SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converted)) &&
            SUCCEEDED(converted->Initialize(source.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(frame->WriteSource(converted.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
}
