#pragma once
#include "ui/ItemImage.h"
#include <memory>
#include <span>
#include <RE/Bethesda/BSExtraData.h>
#include "NativeExtraLease.h"

namespace rock_backpack::item_preview
{
    // Messages/presentation only publish requests. The native dispatch hook
    // owns renderer/tasks and constructs detached clones. After publication,
    // the control task owns clone mutation; the cache only retains references.
    bool install();
    void setSessionReady(bool ready) noexcept;
    void setVisible(bool visible) noexcept;
    struct Request
    {
        std::uint64_t appearance{};
        std::uint32_t form{};
        native_inventory::ExtraLease extra;
        bool scene{}; // A detached physical display model is needed even with the UI closed.
    };
    // Control-task publication. Each key identifies an immutable appearance;
    // quantity and layout do not change it. At most 128 visible appearances.
    void request(std::span<const Request> items);
    RE::NiPointer<RE::NiAVObject> scene(std::uint64_t appearance);
    // Control-task ownership, detached collisionless template from the exact
    // world item before its native transfer retires the source model.
    void publishScene(std::uint64_t appearance,std::uint32_t form,RE::NiPointer<RE::NiAVObject> visual);
    struct Snapshot
    {
        std::shared_ptr<const ui::ItemImage> image;
        const char* status{};
        std::shared_ptr<const std::vector<ui::ItemPicture>> pictures;
    };
    Snapshot snapshot(ID3D11Device* device, std::uint64_t deviceGeneration);
}
