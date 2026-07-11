#pragma once
#include "PanelModel.h"
#include "Icons.h"
#include "ItemImage.h"
#include <optional>
#include <span>
#include <memory>

namespace rock_backpack::ui
{
    void initializeStyle();
    inline constexpr unsigned kButtonWidth = 600, kButtonHeight = 200;
    bool drawInventoryButton(float inventoryFullSeconds = 0.f);
    struct ViewState
    {
        inventory_policy::Category category{ inventory_policy::Category::Aid };
        std::string selected;
        std::uint64_t presentation{};
        std::uint64_t modelGeneration{};
        std::size_t selectedIndex{ static_cast<std::size_t>(-1) };
        storage::ItemId pressed{};
        ImVec2 pressPosition{};
        unsigned offsetX{}, offsetY{};
        bool dragging{}, rotated{};
        bool miscellaneous{};
        bool overflow{};
        bool heldWeapons{};
        Hand dragHand{Hand::None};
        int splitQuantity{1};
    };
    std::optional<Action> drawPanel(const Model& model, ViewState& state,
        const IconTextures& icons, Hand hand, const ItemImage* stimpak = nullptr,
        const char* previewStatus = "Item preview pending", std::span<const ItemPicture> pictures = {});
}
