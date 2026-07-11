#pragma once
#include "NativeInventory.h"
#include "NativeItemPreview.h"
#include "ui/PanelModel.h"
#include "StoredReferenceRecovery.h"
#include "InventoryMirror.h"
#include "InventoryDisplayItem.h"
#include <F4SE/Interfaces.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

namespace rock_backpack
{
    // Native mutations belong to the serialized F4SE control task. Save/load
    // takes the same state lock. The engine identity event only updates atomic
    // numeric keys and never touches engine objects or takes this lock.
    class BackpackStorage final : public RE::BSTEventSink<native_inventory::IdentityChange>
    {
    public:
        static BackpackStorage& get();
        bool initialize();
        bool attachEvents();
        void setContainerBase(std::uint32_t base);
        RE::ObjectRefHandle ensureBackpack(std::uint32_t base, RE::ObjectRefHandle existing = {});
        RE::ObjectRefHandle backpack() const;
        bool onBack() const;
        bool pullBackpack(const RE::NiPoint3& position);
        bool returnBackpack();
        bool select(RE::TESObjectREFR& backpack, std::uint32_t containerBase);
        void leave();
        void beforeLoad();
        std::uint64_t generation() const;
        void restoreStoredReferences();
        std::shared_ptr<ui::Model> model(const std::string& status);
        bool deposit(RE::TESObjectREFR& reference, storage::Location location, bool* capacityRejected = nullptr);
        bool stowEquipped(std::uint32_t form,storage::Location location);
        std::optional<bool> takeStowResult();
        bool stowPending() const { std::scoped_lock lock(_mutex); return _nativeStow.has_value(); }
        void setPolicyActive(bool value) noexcept { _policyActive=value; }
        bool policyActive() const noexcept { return _policyActive.load(); }
        bool nativeUiActive() const noexcept { return _policyActive.load() && _nativeReady.load() && _nativeAccess.load(); }
        bool admitNative(const player_inventory::Row&,RE::ExtraDataList*);
        bool admitNativeBatch(RE::TESObjectREFR&,std::span<const player_inventory::Row>);
        struct NativeIncoming { player_inventory::Row row; native_inventory::ExtraLease extra; };
        struct NativeOutgoing { player_inventory::Row row; std::uint32_t quantity{}; };
        bool admitTrade(std::span<const NativeOutgoing>,std::span<const NativeIncoming>);
        void invalidateNative() noexcept { _nativeDirty=true; }
        std::optional<storage::Kind> kind(storage::ItemId id) const;
        std::uint32_t form(storage::ItemId id) const;
        storage::Result arrange(const ui::Action& action);
        struct Delivery { RE::ObjectRefHandle handle; storage::ItemId id{}; };
        Delivery withdraw(storage::ItemId id, const RE::NiPoint3& position,std::uint64_t expectedGeneration);
        bool completeDelivery(bool success);
        bool use(storage::ItemId id, ui::ActionKind action,std::uint64_t expectedGeneration, player_inventory::Row* transferredWeapon = nullptr);
        bool pending() const;
        bool needsRecovery() const;
        std::uint32_t reference(storage::ItemId id) const;
        std::uint32_t displayContainer() const;
        void refreshWeight(std::uint32_t rightHeld, std::uint32_t leftHeld);
        RE::ObjectRefHandle pendingReference() const;
        std::array<InventoryDisplayItem, 3> mounts(std::array<storage::Kind,3>& kinds) const;
        std::vector<item_preview::Request> previews();
        std::vector<std::uint32_t> storedReferences() const;
        RE::BSEventNotifyControl ProcessEvent(const native_inventory::IdentityChange&, RE::BSTEventSource<native_inventory::IdentityChange>*) override;
    private:
        static constexpr unsigned kBags = 32;
        struct Record {
            storage::ItemId id{};
            std::uint32_t form{}, reference{};
            native_inventory::Identity stack;
            RE::NiPointer<RE::TESObjectREFR> lease;
            std::uint64_t appearance{};
            storage_recovery::State recovery;
            std::optional<storage_recovery::Result> recoveryWarning;
        };
        struct Bag {
            std::uint32_t container{}, reference{};
            RE::NiPointer<RE::TESObjectREFR> containerLease;
            storage::Grid grid{};
            std::vector<Record> records;
        };
        struct Pending {
            unsigned bag{kBags}; storage::ItemId id{};
            std::uint32_t reference{};
            RE::NiPointer<RE::TESObjectREFR> lease;
        };
        BackpackStorage() = default;
        static void save(const F4SE::SerializationInterface*);
        static void load(const F4SE::SerializationInterface*);
        static void revert(const F4SE::SerializationInterface*);
        Record* find(Bag&, storage::ItemId);
        player_inventory::Row row(Bag&, const Record&, const player_inventory::Snapshot* snapshot = nullptr);
        bool restorePending();
        void consume(Bag&, storage::ItemId);
        RE::NiPointer<RE::TESObjectREFR> container(Bag&);
        RE::NiPointer<RE::TESObjectREFR> weaponReference(const Record&) const;
        bool reconcileNative(storage::ItemId debit=0);
        player_inventory::Row nativeRow(storage::ItemId) const;
        bool importLegacy();
        InventoryDisplayItem displayItem(storage::ItemId) const;
        struct Mount { InventoryDisplayItem reference; storage::Kind kind{storage::Kind::Misc}; };
        Mount mounted(storage::Location location) const;
        bool placeNative(storage::ItemId,storage::Placement);
        void saveNative(const F4SE::SerializationInterface*) const;
        bool loadNative(const F4SE::SerializationInterface*,std::uint32_t type,std::uint32_t version,std::uint32_t bytes);
        inventory_mirror::Ledger _native;
        player_inventory::Snapshot _nativeSnapshot;
        std::unordered_map<storage::ItemId,std::size_t> _nativeRowsById;
        std::atomic_bool _nativeReady{},_policyActive{};
        std::uint32_t _nativePendingForm{};
        struct NativeStow { player_inventory::Row row; storage::Location location{}; std::chrono::steady_clock::time_point deadline; };
        std::optional<NativeStow> _nativeStow;
        std::optional<bool> _nativeStowResult;
        void finishNativeStow();
        bool nativeStackHasSpace(const player_inventory::Row&) const;
        bool attachNativeMirrorEvents();
        bool selectBag(RE::TESObjectREFR&,std::uint32_t);
        std::atomic_bool _nativeAccess{};
        std::atomic_bool _nativeDirty{true};
        bool _mirrorEventsRegistered{},_legacyImported{},_layoutDirty{};
        std::chrono::steady_clock::time_point _nextNativeProbe{};
        mutable std::recursive_mutex _mutex;
        std::array<Bag, kBags> _bags;
        std::array<std::atomic_uint64_t, kBags> _identities{};
        std::atomic_uint64_t _transferIdentity{};
        unsigned _active{kBags};
        storage::ItemId _nextId{1};
        std::uint64_t _generation{1};
        Pending _pending;
        bool _initialized{}, _eventsRegistered{}, _loadedPending{}, _managedRecordPresent{};
        std::uint32_t _containerBase{};
        std::uint32_t _backpackReference{}, _backpackBase{};
        RE::NiPointer<RE::TESObjectREFR> _backpackLease;
        std::chrono::steady_clock::time_point _nextWeightUpdate{};
    };
}
