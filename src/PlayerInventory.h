#pragma once

#include <RobCoPALM/Icons.h>
#include "InventoryPolicy.h"
#include "StorageGrid.h"

#include "RE/Bethesda/BSPointerHandle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
    class NiPoint3;
    class TESObjectREFR;
    class ExtraDataList;
}

namespace rock_backpack::player_inventory
{
    struct Revision
    {
        bool available{ false };
        std::uint32_t rowCount{ 0 };
        std::uint64_t hashA{ 0 };
        std::uint64_t hashB{ 0 };

        bool operator==(const Revision&) const = default;
    };

    struct Row
    {
        std::string id;
        std::string name;
        std::string armorSlotLabel;
        inventory_policy::Category category{
            inventory_policy::Category::Armor
        };
        std::uint32_t formId{ 0 };
        std::uint32_t stackId{ 0 };
        std::uint32_t count{ 0 };
        std::uint32_t armorSlotMask{ 0 };
        std::uint32_t armorZoneFlags{ 0 };
        // Per-unit weight resolved through the engine's own form-weight
        // resolver, so mod-adjusted instance weights are already applied.
        float weight{ 0.0f };
        // Resolved natively so the view never has to reason about record data.
        // Uses PALM's item catalog and instance-aware keyword classification.
        palm::api::Icon icon{ palm::api::Icon::OpenHand };
        // Whether the use action means anything for this row. Resolved
        // natively so the view never infers it from the category.
        bool usable{ false };
        bool equipped{ false };

        // Native-only identity witnesses. These values are compared after a
        // fresh locked read and never cross the render boundary.
        std::uintptr_t stackIdentity{ 0 };
        std::uintptr_t instanceIdentity{ 0 };
        std::uint32_t uniqueBase{};
        std::uint16_t uniqueIndex{};
        storage::Kind kind{storage::Kind::Misc};

        bool operator==(const Row&) const = default;
    };

    struct Snapshot
    {
        bool available{ false };
        Revision revision{};
        std::vector<Row> rows;

        bool operator==(const Snapshot&) const = default;
    };

    // Vitals drift continuously (health regenerates, carried weight moves with
    // every transfer), so they are captured and published on their own cadence
    // instead of being folded into Snapshot. Putting them in Snapshot would
    // make its equality check fail constantly and force a full row-list rebuild
    // on every refresh tick.
    struct Vitals
    {
        bool available{ false };
        // False when the carry-capacity actor value could not be resolved or
        // returned an implausible number; the panel then hides the capacity
        // readout rather than showing a value it cannot stand behind.
        bool capacityKnown{ false };
        float carriedWeight{ 0.0f };
        float carryCapacity{ 0.0f };
        float health{ 0.0f };
        float maxHealth{ 0.0f };

        bool operator==(const Vitals&) const = default;
    };

    enum class ArmorActionResult : std::uint8_t
    {
        Equipped,
        Unequipped,
        AlreadyInState,
        PlayerUnavailable,
        InventoryUnavailable,
        StackUnavailable,
        ProtectedArmor,
        EquipManagerUnavailable,
        NativeActionRejected,
    };

    enum class WeaponActionResult : std::uint8_t
    {
        Equipped,
        Unequipped,
        PlayerUnavailable,
        InventoryUnavailable,
        StackUnavailable,
        EquipSlotUnavailable,
        EquipManagerUnavailable,
        NativeActionRejected,
    };

    enum class UseActionResult : std::uint8_t
    {
        Used,
        PlayerUnavailable,
        InventoryUnavailable,
        StackUnavailable,
        NotConsumable,
        EquipManagerUnavailable,
        NativeActionRejected,
    };

    enum class MaterializeActionResult : std::uint8_t
    {
        Materialized,
        PlayerUnavailable,
        InventoryUnavailable,
        StackUnavailable,
        NativeActionRejected,
    };

    struct MaterializeOutcome
    {
        MaterializeActionResult result{
            MaterializeActionResult::NativeActionRejected
        };
        RE::ObjectRefHandle handle{};

        [[nodiscard]] bool succeeded() const noexcept
        {
            return result == MaterializeActionResult::Materialized;
        }
    };

    [[nodiscard]] Revision probeRevision() noexcept;
    [[nodiscard]] Snapshot capture() noexcept;
    [[nodiscard]] Snapshot capture(RE::TESObjectREFR& container) noexcept;
    [[nodiscard]] Row describe(RE::TESObjectREFR& reference) noexcept;
    [[nodiscard]] Vitals captureVitals() noexcept;
    [[nodiscard]] UseActionResult useItem(const Row& expected) noexcept;
    [[nodiscard]] const char* useResultMessage(
        UseActionResult result) noexcept;

    // Removes exactly one revalidated stack unit into the world at
    // dropLocation. Both the panel drop action and the panel force grab build
    // on this: a grab is a materialization at the free hand followed by a ROCK
    // force-grab request against the returned reference.
    [[nodiscard]] MaterializeOutcome materializeRow(
        const Row& expected,
        const RE::NiPoint3& dropLocation) noexcept;
    [[nodiscard]] const char* materializeResultMessage(
        MaterializeActionResult result) noexcept;
    [[nodiscard]] WeaponActionResult toggleWeapon(
        const Row& expected) noexcept;
    [[nodiscard]] const char* weaponResultMessage(
        WeaponActionResult result) noexcept;
    [[nodiscard]] ArmorActionResult setArmorEquipped(
        const Row& expected,
        bool equipped) noexcept;
    [[nodiscard]] const char* armorResultMessage(
        ArmorActionResult result) noexcept;
}
