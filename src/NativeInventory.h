#pragma once
#include "PlayerInventory.h"
#include "NativeExtraLease.h"
#include <RE/Bethesda/BSExtraData.h>
#include <RE/Bethesda/BSTEvent.h>

namespace rock_backpack::native_inventory
{
    struct Identity
    {
        std::uint32_t base{};
        std::uint16_t index{};
        bool operator==(const Identity&) const = default;
        explicit operator bool() const { return base && index; }
    };
    // FO4VR 420360 writes and 13F8400 reads this exact event layout.
    struct IdentityChange
    {
        std::uint32_t oldBase{}, newBase{}, reference{};
        std::uint16_t oldIndex{}, newIndex{};
    };
    static_assert(sizeof(IdentityChange) == 16);
    bool ready();
    Identity identity(RE::ExtraDataList* extra);
    Identity ensureIdentity(RE::TESObjectREFR& reference);
    // Pin exactly one unequipped weapon unit for a physical slot. Ordinary
    // inventory stacks must never receive synthetic IDs just for UI tracking.
    Identity pinWeapon(RE::TESObjectREFR& container,const player_inventory::Row& row);
    std::uint32_t worldCount(RE::TESObjectREFR& reference);
    RE::BSTEventSource<IdentityChange>* identityEvents();
    RE::ObjectRefHandle createContainer(std::uint32_t baseForm);
    RE::ObjectRefHandle createBackpack(std::uint32_t baseForm);
    void backpackSound(std::uint32_t baseForm, bool pickedUp);
    bool deposit(RE::TESObjectREFR& container, RE::TESObjectREFR& reference, bool identify = true);
    RE::ObjectRefHandle withdraw(RE::TESObjectREFR& container, const player_inventory::Row& row,
        std::uint32_t quantity, const RE::NiPoint3& position);
    bool transfer(RE::TESObjectREFR& source, RE::TESObjectREFR& destination,
        const player_inventory::Row& row, std::uint32_t quantity);
    ExtraLease extra(RE::TESObjectREFR& container, const player_inventory::Row& row);
    bool compatible(RE::ExtraDataList* first, RE::ExtraDataList* second);
    bool current(RE::TESObjectREFR&,const player_inventory::Row&);
    bool park(RE::TESObjectREFR& reference);
    // Exact saved-reference loading and owner-scoped native persistence.
    // Serialized storage task only, after a ready game-session snapshot.
    void loadSavedReference(std::uint32_t reference);
    bool retainStoredReference(RE::TESObjectREFR& reference, RE::TESObjectREFR& owner);
    bool releaseStoredReference(RE::TESObjectREFR& reference, RE::TESObjectREFR& owner);
    bool showAt(RE::TESObjectREFR& reference, const RE::NiPoint3& position);
}
