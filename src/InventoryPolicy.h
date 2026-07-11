#pragma once

#include <cstdint>
#include <string_view>

namespace rock_backpack::inventory_policy
{
    enum class Category : std::uint8_t
    {
        Weapons,
        Armor,
        Aid,
        Junk,
        Ammo,
        Misc,
    };

    [[nodiscard]] constexpr std::string_view categoryKey(
        const Category category) noexcept
    {
        switch (category) {
        case Category::Weapons:
            return "weapons";
        case Category::Armor:
            return "armor";
        case Category::Aid:
            return "aid";
        case Category::Junk:
            return "junk";
        case Category::Ammo:
            return "ammo";
        case Category::Misc:
            return "misc";
        default:
            return "unknown";
        }
    }

    enum ArmorZoneFlag : std::uint32_t
    {
        ArmorZone_None = 0,
        ArmorZone_Head = 1u << 0,
        ArmorZone_Torso = 1u << 1,
        ArmorZone_LeftArm = 1u << 2,
        ArmorZone_RightArm = 1u << 3,
        ArmorZone_Legs = 1u << 4,
        ArmorZone_Accessory = 1u << 5,
        ArmorZone_Utility = 1u << 6,
    };

    [[nodiscard]] constexpr std::uint32_t editorIndexMask(
        const std::int32_t index) noexcept
    {
        return index >= 0 && index < 32 ?
                   (1u << static_cast<std::uint32_t>(index)) :
                   0u;
    }

    // These groups are presentation-only. Fallout remains authoritative for
    // actual compatibility through each armor form's full authored slot mask.
    inline constexpr std::uint32_t kHeadMask =
        editorIndexMask(0) | editorIndexMask(1) | editorIndexMask(2) |
        editorIndexMask(16) | editorIndexMask(17) | editorIndexMask(18) |
        editorIndexMask(19) | editorIndexMask(20) | editorIndexMask(22);
    inline constexpr std::uint32_t kTorsoMask =
        editorIndexMask(3) | editorIndexMask(6) | editorIndexMask(11);
    inline constexpr std::uint32_t kLeftArmMask =
        editorIndexMask(4) | editorIndexMask(7) | editorIndexMask(12);
    inline constexpr std::uint32_t kRightArmMask =
        editorIndexMask(5) | editorIndexMask(8) | editorIndexMask(13);
    inline constexpr std::uint32_t kLegsMask =
        editorIndexMask(9) | editorIndexMask(10) |
        editorIndexMask(14) | editorIndexMask(15);
    inline constexpr std::uint32_t kAccessoryMask =
        editorIndexMask(21);
    inline constexpr std::uint32_t kNamedZoneMask =
        kHeadMask | kTorsoMask | kLeftArmMask | kRightArmMask |
        kLegsMask | kAccessoryMask;

    // The paperdoll needs exactly one home bay per equipped piece, but Fallout
    // armor is layered rather than one-item-per-slot: an outfit legitimately
    // covers torso, both arms and both legs at once, and an under-armor piece
    // can share a zone with the armor worn over it. Bays therefore hold a stack
    // of pieces, and this enum names the stack a piece is filed under. It is
    // presentation policy only; the authored slot mask stays authoritative for
    // what the game will actually let the player wear.
    enum class ArmorBay : std::uint8_t
    {
        Head,
        Torso,
        LeftArm,
        RightArm,
        Legs,
        Accessory,
        Utility,
    };

    [[nodiscard]] constexpr std::string_view armorBayKey(
        const ArmorBay bay) noexcept
    {
        switch (bay) {
        case ArmorBay::Head:
            return "head";
        case ArmorBay::Torso:
            return "torso";
        case ArmorBay::LeftArm:
            return "leftArm";
        case ArmorBay::RightArm:
            return "rightArm";
        case ArmorBay::Legs:
            return "legs";
        case ArmorBay::Accessory:
            return "accessory";
        case ArmorBay::Utility:
        default:
            return "utility";
        }
    }

    [[nodiscard]] constexpr std::uint32_t armorZoneFlags(
        const std::uint32_t slotMask) noexcept
    {
        std::uint32_t result = ArmorZone_None;
        if ((slotMask & kHeadMask) != 0) {
            result |= ArmorZone_Head;
        }
        if ((slotMask & kTorsoMask) != 0) {
            result |= ArmorZone_Torso;
        }
        if ((slotMask & kLeftArmMask) != 0) {
            result |= ArmorZone_LeftArm;
        }
        if ((slotMask & kRightArmMask) != 0) {
            result |= ArmorZone_RightArm;
        }
        if ((slotMask & kLegsMask) != 0) {
            result |= ArmorZone_Legs;
        }
        if ((slotMask & kAccessoryMask) != 0) {
            result |= ArmorZone_Accessory;
        }
        if (slotMask == 0 || (slotMask & ~kNamedZoneMask) != 0) {
            result |= ArmorZone_Utility;
        }
        return result;
    }

    [[nodiscard]] constexpr std::uint32_t armorZoneCount(
        const std::uint32_t zoneFlags) noexcept
    {
        std::uint32_t count = 0;
        for (std::uint32_t bit = 1; bit <= ArmorZone_Utility; bit <<= 1) {
            if ((zoneFlags & bit) != 0) {
                ++count;
            }
        }
        return count;
    }

    // Files a multi-zone piece under a single bay, widest garment first: a suit
    // covering torso, arms and legs reads as body armor, while a limb-only or
    // head-only piece keeps its own bay. Callers show the full authored zone
    // set alongside the bay so nothing about the real coverage is hidden.
    [[nodiscard]] constexpr ArmorBay primaryArmorBay(
        const std::uint32_t zoneFlags) noexcept
    {
        if ((zoneFlags & ArmorZone_Torso) != 0) {
            return ArmorBay::Torso;
        }
        if ((zoneFlags & ArmorZone_Legs) != 0) {
            return ArmorBay::Legs;
        }
        if ((zoneFlags & ArmorZone_Head) != 0) {
            return ArmorBay::Head;
        }
        if ((zoneFlags & ArmorZone_LeftArm) != 0) {
            return ArmorBay::LeftArm;
        }
        if ((zoneFlags & ArmorZone_RightArm) != 0) {
            return ArmorBay::RightArm;
        }
        if ((zoneFlags & ArmorZone_Accessory) != 0) {
            return ArmorBay::Accessory;
        }
        return ArmorBay::Utility;
    }
}
