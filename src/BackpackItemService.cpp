#include "PCH.h"

#include "BackpackItemService.h"

#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BSLock.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESObjectREFRs.h"

namespace rock_backpack::backpack_item
{
    namespace
    {
        [[nodiscard]] RE::TESBoundObject* resolveObject(
            const std::uint32_t baseFormId) noexcept
        {
            return baseFormId != 0 ?
                       RE::TESForm::GetFormByID<RE::TESBoundObject>(
                           baseFormId) :
                       nullptr;
        }

        [[nodiscard]] std::optional<std::uint32_t> firstNonEmptyStack(
            RE::PlayerCharacter& player,
            const RE::TESBoundObject& object) noexcept
        {
            if (!player.inventoryList) {
                return std::nullopt;
            }

            const RE::BSAutoReadLock inventoryLock{
                player.inventoryList->rwLock
            };
            for (const auto& item : player.inventoryList->data) {
                if (item.object != &object) {
                    continue;
                }
                std::uint32_t stackId = 0;
                for (auto* stack = item.stackData.get();
                     stack;
                     stack = stack->nextStack.get(), ++stackId) {
                    if (stack->GetCount() != 0) {
                        return stackId;
                    }
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
    }

    std::uint32_t inventoryCount(const std::uint32_t baseFormId) noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* object = resolveObject(baseFormId);
        return player && object ?
                   player->GetInventoryObjectCount(object) :
                   0;
    }

    MaterializeResult materializeFromInventory(
        const std::uint32_t baseFormId,
        const RE::NiPoint3& dropLocation) noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* object = resolveObject(baseFormId);
        if (!player) {
            return { .reason = "missing-player" };
        }
        if (!object) {
            return { .reason = "missing-base-object" };
        }
        if (!player->inventoryList) {
            return { .reason = "missing-inventory-list" };
        }

        /*
         * Selection is completed under the inventory read lock. RemoveItem is
         * intentionally called only after firstNonEmptyStack has returned and
         * destroyed that lock.
         */
        const auto stackId = firstNonEmptyStack(*player, *object);
        if (!stackId) {
            return { .reason = "inventory-stack-not-found" };
        }

        RE::TESObjectREFR::RemoveItemData removeData(object, 1);
        removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
        removeData.dropLoc = &dropLocation;
        removeData.stackData.push_back(*stackId);

        const RE::ObjectRefHandle handle = player->RemoveItem(removeData);
        if (!handle) {
            return { .reason = "remove-item-failed" };
        }
        return {
            .success = true,
            .handle = handle,
            .reason = handle.get() ?
                          "materialized" :
                          "materialized-reference-pending",
        };
    }

}
