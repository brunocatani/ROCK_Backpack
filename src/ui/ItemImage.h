#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <cstdint>
#include <memory>

namespace rock_backpack::ui
{
    struct ImagePixels;
    inline constexpr std::uint32_t kStimpakForm = 0x00023736;
    struct ItemImage
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        unsigned width{}, height{};
        bool copy(ID3D11DeviceContext* context, ID3D11Texture2D* source);
        bool copyRenderTarget(ID3D11DeviceContext* context);
        bool load(ID3D11Device* device, const std::filesystem::path& path, bool thumbnail = false);
        bool upload(ID3D11Device* device, const ImagePixels& pixels);
        bool belongsTo(ID3D11Device* device) const;
    };
    struct ImagePixels
    {
        unsigned width{}, height{}, stride{};
        DXGI_FORMAT format{};
        std::vector<std::uint8_t> bytes;
    };
    struct ItemPicture
    {
        std::uint64_t appearance{};
        std::shared_ptr<const ItemImage> image;
    };
    struct ImageCoverage
    {
        bool valid{};
        std::uint32_t colorPixels{}, alphaPixels{}, visiblePixels{};
    };
    ImageCoverage inspectItemImage(const ImagePixels& pixels);
    ImagePixels itemThumbnail(const ImagePixels& pixels, unsigned maximumSize = 256);
    // Poll only on the capture context's thread. Pending never waits for the GPU.
    enum class ReadbackResult { Pending, Ready, Failed };
    class ImageReadback
    {
    public:
        bool begin(ID3D11DeviceContext* context, const ItemImage& image);
        ReadbackResult poll(ID3D11DeviceContext* context, ImagePixels& pixels);
        void clear() { _staging.Reset(); }
    private:
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _staging;
    };
    // Filesystem/WIC work belongs to a control task, never a render callback.
    std::filesystem::path stimpakImagePath();
    bool saveItemImage(const ImagePixels& pixels, const std::filesystem::path& path);
}
