#pragma once

#include "InventoryPolicy.h"
#include <RobCoPALM/Icons.h>
#include "StorageGrid.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace rock_backpack::ui
{
    inline constexpr unsigned kWidth = 1000, kHeight = 750;
    struct Item
    {
        std::string id, name, slots;
        inventory_policy::Category category{};
        palm::api::Icon icon{ palm::api::Icon::OpenHand };
        std::uint32_t count{}, zones{};
        float weight{};
        bool equipped{}, usable{};
        std::uint32_t formId{};
        storage::ItemId storageId{};
        std::uint64_t appearance{};
    };
    struct Model
    {
        std::uint64_t generation{};
        bool available{}, vitalsAvailable{}, capacityKnown{};
        float weight{}, capacity{}, health{}, maxHealth{};
        std::string status;
        std::vector<Item> items;
        std::array<std::vector<std::size_t>, 6> categories;
        std::vector<std::size_t> equipped;
        std::vector<std::size_t> miscellaneous;
        std::vector<std::size_t> overflow;
        std::vector<std::size_t> heldWeapons;
        std::unordered_map<storage::ItemId,std::size_t> itemByStorageId;
        storage::Grid grid{};

        // Built on the publishing task. Rendering visits only clipped visible
        // rows, irrespective of the player's total inventory size.
        void indexItems()
        {
            for (auto& category : categories) category.clear();
            equipped.clear();
            miscellaneous.clear(); overflow.clear(); heldWeapons.clear(); itemByStorageId.clear();
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto category = static_cast<std::size_t>(items[i].category);
                if (category < categories.size()) categories[category].push_back(i);
                if (items[i].category == inventory_policy::Category::Armor && items[i].equipped) equipped.push_back(i);
                itemByStorageId.emplace(items[i].storageId,i);
            }
            for (const auto& entry : grid.entries()) if (entry.kind == storage::Kind::Misc)
                if (const auto item = itemByStorageId.find(entry.id); item != itemByStorageId.end()) miscellaneous.push_back(item->second);
            for(const auto& entry:grid.entries()) if(entry.kind!=storage::Kind::Misc && entry.placement.location==storage::Location::Overflow)
                if(const auto item=itemByStorageId.find(entry.id);item!=itemByStorageId.end()) overflow.push_back(item->second);
            for(const auto& entry:grid.entries()) if(entry.placement.location==storage::Location::Equipped)
                if(const auto item=itemByStorageId.find(entry.id);item!=itemByStorageId.end()) heldWeapons.push_back(item->second);
        }
    };
    enum class ActionKind { Grab, Drop, Use, ToggleWeapon, EquipArmor, UnequipArmor, Move, Split, Merge, Close, Open };
    enum class Hand { None, Left, Right };
    struct Action
    {
        ActionKind kind{};
        std::uint64_t generation{}, presentation{};
        std::array<char, 65> rowId{};
        Hand hand{};
        storage::ItemId target{};
        storage::Placement destination{};
        std::uint32_t quantity{1};
        bool exactHand{};
    };
}
