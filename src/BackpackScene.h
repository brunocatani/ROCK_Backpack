#pragma once
#include "BackpackZones.h"
#include "InventoryDisplayItem.h"
#include "WeaponMaterialFix.h"
#include "WeaponTransition.h"
#include <RE/Bethesda/BSPointerHandle.h>
#include <RE/NetImmerse/NiNode.h>
#include <chrono>
#include <cstdio>
#include <optional>

namespace rock_backpack
{
    // The serialized control task owns detached mounted model clones.
    // Native inventory instances remain item authority.
    class BackpackScene
    {
    public:
        static RE::NiPointer<RE::NiAVObject> cloneVisual(RE::NiAVObject* source);
        static std::array<char,64> displayName(std::uint64_t reference)
        {
            std::array<char,64> name{};
            std::snprintf(name.data(),name.size(),"ROCKBackpack.NativeWeapon.%llu",static_cast<unsigned long long>(reference));
            return name;
        }
        // Places the three mount visuals; elapsedSeconds advances their eases.
        void update(RE::TESObjectREFR& backpack, const std::array<InventoryDisplayItem, 3>& mounts, const zones::Layout& layout,
            const std::array<storage::Kind,3>& kinds, float elapsedSeconds);
        // A weapon was just put on a mount: when its visual appears there, it
        // eases in from rootLocal (the backpack root space pose it left from).
        void expectArrival(storage::Location location, std::uint32_t form, const panel_math::StoredTransform& rootLocal);

        // Draw stand-ins, per physical hand (0 right, 1 left). The mount clone
        // of `item` moves under the hand's wand node without a jump. A firearm
        // then eases into the firing aim given to updateStandIn; melee stays as
        // grabbed. The clone is no longer shown on its mount meanwhile.
        bool beginStandIn(unsigned hand, storage::ItemId item, RE::NiNode& wand, bool followsAim);
        // hidden: ROCK shows the real weapon in that hand.
        void updateStandIn(unsigned hand, const std::optional<panel_math::StoredTransform>& aimLocal, bool hidden, float elapsedSeconds);
        // returnToMount: the draw failed and the weapon is on its mount again;
        // its visual eases back there from the hand.
        void endStandIn(unsigned hand, bool returnToMount);
        bool standInActive(unsigned hand) const { return hand<_standIns.size() && _standIns[hand].object; }
        float standInSeconds(unsigned hand) const
        {
            if(!standInActive(hand)) return 0.f;
            return std::chrono::duration<float>(std::chrono::steady_clock::now()-_standIns[hand].started).count();
        }

        void clearBackpack();
        struct DisplayBinding { std::uint32_t parent{},reference{}; std::uint64_t id{}; InventoryDisplayItem item; };
        std::array<DisplayBinding,3> displays(std::uint32_t backpack) const;
        void clear(bool sceneAvailable);
    private:
        struct Model {
            InventoryDisplayItem item;
            RE::NiPointer<RE::NiAVObject> object;
            weapon_material_fix::Preparation materials;
            bool contactReady{};
        };
        struct Arrival {
            std::uint32_t form{};
            panel_math::StoredTransform local;
            std::chrono::steady_clock::time_point at{};
            bool valid{};
        };
        struct StandIn {
            RE::NiPointer<RE::NiAVObject> object;
            RE::NiPointer<RE::NiNode> wand;
            storage::ItemId item{};
            std::uint32_t form{};
            storage::Location location{storage::Location::Interior};
            weapon_transition::Ease ease;
            float scale{1.f}; // local scale under the wand at the grab
            bool followsAim{};
            std::chrono::steady_clock::time_point started{};
        };
        Model* resolveModel(const InventoryDisplayItem& item);
        bool inHand(storage::ItemId item) const;
        // Three mounts plus a stand-in in each hand.
        std::array<Model, 5> _models{};
        std::array<RE::NiPointer<RE::NiAVObject>, 3> _mounted;
        std::array<weapon_transition::Ease, 3> _mountEases;
        std::array<Arrival, 3> _arrivals;
        std::array<StandIn, 2> _standIns;
        RE::NiPointer<RE::NiNode> _parent;
    };
}
