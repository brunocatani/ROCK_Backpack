#include "PCH.h"
#include "InventoryCapacity.h"
#include "BackpackStorage.h"
#include "BackpackWeight.h"
#include "BackpackItemService.h"
#include "BackpackScene.h"
#include <algorithm>
#include <cstring>
#include <charconv>

namespace rock_backpack
{
    namespace
    {
        namespace native = native_inventory;
        std::uint64_t key(native::Identity id) { return (std::uint64_t(id.base) << 16) | id.index; }
        native::Identity identity(std::uint64_t value) { return {std::uint32_t(value >> 16), std::uint16_t(value)}; }
        RE::NiPointer<RE::TESObjectREFR> resolve(std::uint32_t id)
        {
            auto* ref = id ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(id) : nullptr;
            return ref && !ref->IsDeleted() ? RE::NiPointer<RE::TESObjectREFR>(ref) : RE::NiPointer<RE::TESObjectREFR>{};
        }
        constexpr std::uint32_t bagRecord = 0x47414252, pendingRecord = 0x4E455052, managedRecord = 0x4B50424D;
        struct SavedBag {
            std::uint64_t identity{};
            std::uint32_t container{}, reference{}, columns{}, rows{}, count{}, policy{2};
        };
        struct SavedItem {
            std::uint64_t id{}, appearance{};
            std::uint32_t form{}, reference{}, stackBase{}, quantity{}, x{}, y{};
            std::uint16_t stackIndex{};
            std::uint8_t kind{}, location{}, rotated{}, reserved[3]{};
        };
        static_assert(sizeof(SavedBag) == 32 && sizeof(SavedItem) == 48);
    }
    BackpackStorage& BackpackStorage::get() { static BackpackStorage value; return value; }
    void BackpackStorage::setContainerBase(std::uint32_t base)
    { std::scoped_lock lock(_mutex); _containerBase = base; }
    RE::ObjectRefHandle BackpackStorage::backpack() const
    {
        std::scoped_lock lock(_mutex);
        const auto ref = _backpackLease ? _backpackLease : resolve(_backpackReference);
        return ref && ref->GetObjectReference() && ref->GetObjectReference()->formID == _backpackBase ? ref->GetHandle() : RE::ObjectRefHandle{};
    }
    bool BackpackStorage::onBack() const
    { const auto ref = backpack().get(); return ref && ref->IsDisabled(); }
    RE::ObjectRefHandle BackpackStorage::ensureBackpack(std::uint32_t base, RE::ObjectRefHandle existing)
    {
        std::scoped_lock lock(_mutex); _backpackBase = base;
        if (_managedRecordPresent && !_backpackReference) return {};
        if (_backpackReference) {
            if (!_backpackLease) _backpackLease = resolve(_backpackReference);
            return backpack(); // Missing saved references never authorize a replacement item.
        }
        auto* player = RE::PlayerCharacter::GetSingleton(); if (!player) return {};
        auto ref = existing.get();
        bool migrated{};
        if (!ref && backpack_item::inventoryCount(base)) {
            // One-time adoption of the old inventory-backed bag. All later
            // pulls use this exact reference without native Add/RemoveItem.
            const auto result = backpack_item::materializeFromInventory(base, player->GetPosition());
            ref = result.handle.get();
            migrated = bool(ref);
        }
        if (!ref) for (auto& bag : _bags) if (bag.reference) {
            const auto candidate = resolve(bag.reference);
            if (candidate && candidate->GetObjectReference() && candidate->GetObjectReference()->formID == base) { ref = candidate; break; }
        }
        if (!ref) ref = native::createBackpack(base).get();
        if (!ref || !ref->GetObjectReference() || ref->GetObjectReference()->formID != base) return {};
        _backpackReference = ref->formID; _backpackLease = ref; _managedRecordPresent = true;
        if (migrated && !native::park(*ref)) logger::warn("Legacy backpack adoption awaits parking: {:08X}", ref->formID);
        logger::info("Managed backpack adopted: ref={:08X}; persistent world ownership, no routine inventory cycling", ref->formID);
        return ref->GetHandle();
    }
    bool BackpackStorage::pullBackpack(const RE::NiPoint3& position)
    {
        std::scoped_lock lock(_mutex); const auto ref = backpack().get();
        return ref && ref->IsDisabled() && native::showAt(*ref, position);
    }
    bool BackpackStorage::returnBackpack()
    {
        std::scoped_lock lock(_mutex); const auto ref = backpack().get();
        return ref && (ref->IsDisabled() || native::park(*ref));
    }
    void BackpackStorage::refreshWeight(std::uint32_t rightHeld, std::uint32_t leftHeld)
    {
        std::scoped_lock lock(_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now < _nextWeightUpdate) return;
        _nextWeightUpdate = now + std::chrono::milliseconds(250);
        (void)importLegacy();
        if(_nativeDirty.exchange(false) || _layoutDirty || _nativeStow || now>=_nextNativeProbe) {
            _nextNativeProbe=now+std::chrono::seconds(1);
            (void)reconcileNative();
        }
        finishNativeStow();
        const auto& inventory = _nativeSnapshot;
        float weight{};
        bool shellCounted{};
        for (unsigned i = 0; i < kBags; ++i) {
            const auto uid = _identities[i].load(); if (!uid) continue;
            const auto& bag = _bags[i];
            const bool held = bag.reference && (bag.reference == rightHeld || bag.reference == leftHeld);
            const bool owned = bag.reference && bag.reference == _backpackReference;
            const bool carried = std::any_of(inventory.rows.begin(), inventory.rows.end(), [uid](const auto& item) {
                return key({item.uniqueBase, item.uniqueIndex}) == uid;
            });
            if (!owned && !held && !carried) continue;
            if (const auto backing = container(_bags[i])) weight += backpack_weight::container(*backing);
            for (const auto& record : bag.records) if (record.reference) {
                if (const auto ref = weaponReference(record)) weight += backpack_weight::reference(*ref);
            }
            // A bag in a physical hand is outside the player's inventory, so
            // its own shell must be included here exactly once as well.
            if ((held || owned) && !carried && !shellCounted) if (const auto ref = resolve(bag.reference)) {
                weight += backpack_weight::reference(*ref); shellCounted = true;
            }
            if (_pending.id && _pending.bag == i) {
                const auto* record = find(_bags[i], _pending.id);
                if (record && !record->reference) if (const auto ref = resolve(_pending.reference)) weight += backpack_weight::reference(*ref);
            }
        }
        if (!shellCounted) if (const auto ref = backpack().get()) weight += backpack_weight::reference(*ref);
        backpack_weight::publish(weight);
    }
    bool BackpackStorage::initialize()
    {
        std::scoped_lock lock(_mutex);
        if (_initialized) return true;
        const auto* serial = F4SE::GetSerializationInterface();
        if (!serial || !native::ready()) return false;
        serial->SetUniqueID(0x4B504252); // RBPK, this plugin's own co-save identity.
        serial->SetSaveCallback(save); serial->SetLoadCallback(load); serial->SetRevertCallback(revert);
        _initialized = true;
        logger::info("Backpack storage serialization registered");
        return true;
    }
    bool BackpackStorage::attachEvents()
    {
        std::scoped_lock lock(_mutex);
        if (!attachNativeMirrorEvents()) return false;
        if (_eventsRegistered) return true;
        if (!_initialized) return false;
        // Native event construction and its sink-array allocation need the
        // game's heap. F4SEPlugin_Load occurs before that startup boundary.
        auto* events = native::identityEvents(); if (!events) return false;
        events->RegisterSink(this); _eventsRegistered = true;
        logger::info("Backpack native identity events attached after game data ready");
        return true;
    }
    RE::BSEventNotifyControl BackpackStorage::ProcessEvent(const native::IdentityChange& event, RE::BSTEventSource<native::IdentityChange>*)
    {
        const auto before = key({event.oldBase, event.oldIndex}), after = key({event.newBase, event.newIndex});
        if (before && after) {
            for (auto& current : _identities) { auto expected = before; current.compare_exchange_strong(expected, after); }
            auto expected = before; _transferIdentity.compare_exchange_strong(expected, after);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::NiPointer<RE::TESObjectREFR> BackpackStorage::container(Bag& bag)
    {
        if (!bag.containerLease) bag.containerLease = resolve(bag.container);
        const auto* base = bag.containerLease ? bag.containerLease->GetObjectReference() : nullptr;
        if (!base || ! _containerBase || base->formID != _containerBase || base->GetFormType() != RE::ENUM_FORM_ID::kCONT) return {};
        return bag.containerLease;
    }
    RE::NiPointer<RE::TESObjectREFR> BackpackStorage::weaponReference(const Record& record) const
    {
        auto ref = record.lease ? record.lease : resolve(record.reference);
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        return ref && !ref->IsDeleted() && base && base->formID == record.form && base->GetFormType() == RE::ENUM_FORM_ID::kWEAP ? ref : RE::NiPointer<RE::TESObjectREFR>{};
    }
    BackpackStorage::Record* BackpackStorage::find(Bag& bag, storage::ItemId id)
    {
        for (auto& record : bag.records) if (record.id == id) return &record;
        return nullptr;
    }
    bool BackpackStorage::select(RE::TESObjectREFR& backpack, std::uint32_t containerBase)
    {
        std::scoped_lock lock(_mutex);
        try { const bool selected=selectBag(backpack,containerBase); _nativeAccess=selected; return selected; }
        catch(...) { _nativeAccess=false; throw; }
    }
    bool BackpackStorage::selectBag(RE::TESObjectREFR& backpack, std::uint32_t containerBase)
    {
        std::scoped_lock lock(_mutex);
        if (!_initialized || backpack.IsDeleted()) return false;
        _containerBase = containerBase;
        // The exact world reference is the stable identity. Legacy inventory
        // cycling produced several UID keys for that same bag; prefer its
        // populated ledger, never create another empty inventory on a pull.
        unsigned recovered = kBags;
        for (unsigned i = 0; i < kBags; ++i) if (_bags[i].reference == backpack.formID && _identities[i].load()) {
            if (recovered == kBags || _bags[i].records.size() > _bags[recovered].records.size()) recovered = i;
        }
        if (recovered != kBags) {
            if (_active != recovered) {
                _active = recovered; ++_generation;
                logger::info("Backpack ledger restored by exact reference: ref={:08X}, container={:08X}, entries={}",
                    backpack.formID, _bags[recovered].container, _bags[recovered].records.size());
            }
            return bool(container(_bags[recovered]));
        }
        const auto bagIdentity = native::ensureIdentity(backpack);
        if (!bagIdentity) return false;
        const auto value = key(bagIdentity);
        for (unsigned i = 0; i < kBags; ++i) if (_identities[i].load() == value) {
            _bags[i].reference = backpack.formID;
            if (_active != i) { _active = i; ++_generation; }
            return bool(container(_bags[i]));
        }
        for (unsigned i = 0; i < kBags; ++i) if (!_identities[i].load()) {
            const auto handle = native::createContainer(containerBase);
            auto backing = handle.get(); if (!backing) return false;
            _bags[i] = {}; _bags[i].container = backing->formID; _bags[i].containerLease = backing;
            _bags[i].reference = backpack.formID; _identities[i] = value; _active = i;
            ++_generation; return true;
        }
        logger::warn("Backpack storage refused a new bag: all {} bag identities are in use", kBags);
        return false;
    }
    void BackpackStorage::leave() { std::scoped_lock lock(_mutex); _nativeAccess=false; _active = kBags; ++_generation; }
    void BackpackStorage::beforeLoad()
    {
        std::scoped_lock lock(_mutex); _nativeAccess=false; _active = kBags;
        _nativeReady=false; _nativeSnapshot={}; _nativeRowsById.clear(); _nativeStow.reset(); _nativeStowResult.reset(); _nativeDirty=true; _nextNativeProbe={};
        for(auto& record:_native.records) { record.observed.stackWitness=0; record.observed.instanceWitness=0; }
        backpack_weight::publish(0); _nextWeightUpdate = {};
        for (auto& bag : _bags) {
            bag.containerLease.reset();
            for (auto& record : bag.records) { record.lease.reset(); record.recovery={}; record.recoveryWarning.reset(); }
        }
        _pending.lease.reset(); _backpackLease.reset(); ++_generation;
    }
    std::uint64_t BackpackStorage::generation() const { std::scoped_lock lock(_mutex); return _generation; }
    void BackpackStorage::restoreStoredReferences()
    {
        std::scoped_lock lock(_mutex);
        if(_active==kBags) return;
        auto& bag=_bags[_active];
        const auto backing=container(bag); if(!backing) return;
        struct NativeRecovery
        {
            Record& record;
            RE::TESObjectREFR& owner;
            RE::NiPointer<RE::TESObjectREFR> lookup(std::uint32_t id) const
            { return RE::NiPointer<RE::TESObjectREFR>(RE::TESForm::GetFormByID<RE::TESObjectREFR>(id)); }
            void loadSaved(std::uint32_t id) const { native::loadSavedReference(id); }
            bool validWeapon(const RE::NiPointer<RE::TESObjectREFR>& ref) const
            { return !ref->IsDeleted() && ref->GetObjectReference() && ref->GetObjectReference()->Is(RE::ENUM_FORM_ID::kWEAP); }
            std::uint32_t base(const RE::NiPointer<RE::TESObjectREFR>& ref) const { return ref->GetObjectReference()->formID; }
            bool retain(const RE::NiPointer<RE::TESObjectREFR>& ref) const { return native::retainStoredReference(*ref,owner); }
            void accept(const RE::NiPointer<RE::TESObjectREFR>& ref) { record.lease=ref; }
        };
        for(auto& record:bag.records) {
            if(!record.reference || record.recovery.retained) continue;
            NativeRecovery engine{record,*backing};
            const auto result=storage_recovery::restore(record.reference,record.form,record.recovery,engine);
            if(result==storage_recovery::Result::Retained) {
                ++_generation;
                logger::info("Backpack stored weapon retained: bag={:08X} item={} ref={:08X} base={:08X} restoredFromSave={}",
                    bag.reference,record.id,record.reference,record.form,record.recovery.loadAttempted);
                record.recoveryWarning.reset();
            } else if(record.recoveryWarning!=result) {
                const char* reason=result==storage_recovery::Result::Missing?"reference missing from loaded/native saved state":
                    result==storage_recovery::Result::InvalidReference?"reference deleted or not a weapon":
                    result==storage_recovery::Result::WrongBase?"native base does not match saved base":"native persistence rejected";
                logger::error("Backpack stored weapon unavailable: bag={:08X} item={} ref={:08X} base={:08X} reason={}; ledger preserved",
                    bag.reference,record.id,record.reference,record.form,reason);
                record.recoveryWarning=result;
            }
        }
    }
    bool BackpackStorage::pending() const { std::scoped_lock lock(_mutex); return _pending.id != 0; }
    bool BackpackStorage::needsRecovery() const { std::scoped_lock lock(_mutex); return _loadedPending; }
    std::uint32_t BackpackStorage::reference(storage::ItemId id) const
    {
        std::scoped_lock lock(_mutex);
        return _native.find(id) && _active<kBags?_bags[_active].container:0;
    }
    RE::ObjectRefHandle BackpackStorage::pendingReference() const
    {
        std::scoped_lock lock(_mutex);
        const auto ref = resolve(_pending.reference); return ref ? ref->GetHandle() : RE::ObjectRefHandle{};
    }
    std::uint32_t BackpackStorage::displayContainer() const
    { std::scoped_lock lock(_mutex); return _active<kBags?_bags[_active].container:0; }
    player_inventory::Row BackpackStorage::row(Bag& bag, const Record& record, const player_inventory::Snapshot* source)
    {
        if (record.reference) {
            const auto ref = weaponReference(record);
            return ref ? player_inventory::describe(*ref) : player_inventory::Row{};
        }
        const auto backing = container(bag);
        if (!backing) return {};
        const auto snapshot = source ? player_inventory::Snapshot{} : player_inventory::capture(*backing);
        for (const auto& value : (source ? source->rows : snapshot.rows)) if (value.formId == record.form && value.uniqueBase == record.stack.base && value.uniqueIndex == record.stack.index) return value;
        return {};
    }
    std::shared_ptr<ui::Model> BackpackStorage::model(const std::string& status)
    {
        std::scoped_lock lock(_mutex);
        if(_layoutDirty) (void)reconcileNative();
        auto result=std::make_shared<ui::Model>();
        result->generation=_generation; result->status=status;
        if(!_legacyImported && std::any_of(_bags.begin(),_bags.end(),[](const auto& bag){return !bag.records.empty();}))
            result->status=status+" | Some older backpack items are still unavailable.";
        result->available=_nativeReady && _nativeAccess && _active<kBags;
        if(!result->available) return result;
        result->grid=_native.grid;
        for(const auto& record:_native.records) {
            const auto* entry=_native.grid.find(record.id); if(!entry) continue;
            const auto value=nativeRow(record.id); if(!value.formId) continue;
            ui::Item item;
            item.id=std::to_string(record.id); item.storageId=record.id; item.appearance=record.appearance;
            item.formId=value.formId; item.name=value.name; item.category=value.category; item.icon=value.icon;
            item.count=entry->count; item.weight=value.weight; item.usable=value.usable; item.equipped=value.equipped && (!storage::isWeapon(entry->kind) || entry->placement.location==storage::Location::Equipped);
            item.slots=value.armorSlotLabel; item.zones=value.armorZoneFlags;
            result->weight+=item.weight*item.count; result->items.push_back(std::move(item));
        }
        result->indexItems(); return result;
    }
    std::vector<std::uint32_t> BackpackStorage::storedReferences() const
    {
        std::scoped_lock lock(_mutex); std::vector<std::uint32_t> result;
        if(_active<kBags && _bags[_active].container) result.push_back(_bags[_active].container);
        // Retain claims only for legacy references not yet imported.
        for(const auto& bag:_bags) for(const auto& record:bag.records) if(record.reference) result.push_back(record.reference);
        return result;
    }
    std::vector<item_preview::Request> BackpackStorage::previews()
    {
        std::scoped_lock lock(_mutex);
        std::vector<item_preview::Request> result;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!_nativeReady || !player) return result;
        // Physical mounts first so a large inventory cannot starve its models.
        for(unsigned pass=0;pass<2;++pass) for(const auto& record:_native.records) {
            if(result.size()==128) return result;
            const auto* entry=_native.grid.find(record.id); if(!entry) continue;
            const bool mounted=storage::isMount(entry->placement.location);
            if(mounted!=(pass==0)) continue;
            const auto value=nativeRow(record.id); if(!value.formId) continue;
            result.push_back({record.appearance,value.formId,native::extra(*player,value),mounted});
        }
        return result;
    }
    bool BackpackStorage::deposit(RE::TESObjectREFR& reference, storage::Location location, bool* capacityRejected)
    {
        std::scoped_lock lock(_mutex);
        if(capacityRejected) *capacityRejected=false;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || _active==kBags || _pending.id || _nativeStow || !reconcileNative()) return false;
        const RE::NiPointer<RE::TESObjectREFR> lease(&reference);
        const auto value=player_inventory::describe(reference);
        if(!value.formId || !value.count || reference.formID==_backpackReference) return false;
        std::optional<storage::Placement> destination;
        bool merging{};
        if(location==storage::Location::Interior) {
            for(const auto& current:_nativeSnapshot.rows) if(current.formId==value.formId && nativeStackHasSpace(current)) {
                const auto extra=native::extra(*player,current);
                if(native::compatible(extra.get(),reference.extraList.get())) { merging=true; break; }
            }
        }
        const auto admission=inventory_capacity::forDestination(_native.grid,value.kind,location,merging);
        destination=admission.placement;
        if(!admission.accepted) { if(capacityRejected) *capacityRejected=true; return false; }
        if((!merging && (!destination || _native.grid.fits(value.kind,*destination)!=storage::Result::Ok)) ||
            (storage::isMount(location) && value.count!=1)) {
            if(capacityRejected) *capacityRejected=true;
            return false;
        }
        const bool mounted=storage::isMount(location);
        // A physical slot needs a single durable identity. Ordinary acquisitions
        // keep native extras unchanged so compatible stacks can coalesce.
        _transferIdentity=mounted?key(native::ensureIdentity(reference)):0;
        if(mounted && !_transferIdentity.load()) return false;
        const auto visual=mounted?BackpackScene::cloneVisual(reference.Get3D()):RE::NiPointer<RE::NiAVObject>{};
        const bool accepted=native::deposit(*player,reference,mounted);
        const auto uid=identity(_transferIdentity.load());
        (void)reconcileNative();
        if(!accepted) return false;
        if(mounted) for(const auto& r:_native.records) if(r.observed.form==value.formId &&
            r.observed.uniqueBase==uid.base && r.observed.uniqueIndex==uid.index) {
            const auto id=r.id; const auto appearance=r.appearance;
            if(placeNative(id,*destination)) {
                item_preview::publishScene(appearance,value.formId,visual);
                logger::info("Physical mount transfer confirmed: form={:08X}, item={}, slot={}",value.formId,id,unsigned(location));
                ++_generation; return true;
            }
            break;
        }
        if(mounted) {
            // Never report a completed holster while its item is only in the
            // player inventory. Retry assignment against the durable native UID.
            auto expected=value; expected.uniqueBase=uid.base; expected.uniqueIndex=uid.index; expected.equipped=false;
            _nativeStow=NativeStow{expected,location,std::chrono::steady_clock::now()+std::chrono::seconds(3)};
            logger::warn("Physical mount assignment pending: form={:08X}, uid={:08X}:{}, slot={}; native item preserved",value.formId,uid.base,uid.index,unsigned(location));
        }
        return true;
    }
    storage::Result BackpackStorage::arrange(const ui::Action& action)
    {
        std::scoped_lock lock(_mutex);
        if(!reconcileNative() || _pending.id || _nativeStow || action.generation!=_generation) return storage::Result::Stale;
        storage::ItemId id{};
        const auto end=action.rowId.data()+strnlen(action.rowId.data(),action.rowId.size());
        const auto parsed=std::from_chars(action.rowId.data(),end,id);
        if(parsed.ec!=std::errc{} || parsed.ptr!=end) return storage::Result::Missing;
        auto* record=_native.find(id); if(!record) return storage::Result::Missing;
        auto result=storage::Result::Incompatible;
        if(action.kind==ui::ActionKind::Move && action.destination.location!=storage::Location::Overflow && action.destination.location!=storage::Location::Equipped) {
            if(placeNative(id,action.destination)) result=storage::Result::Ok;
        } else if(action.kind==ui::ActionKind::Split && !storage::isMount(action.destination.location) && action.destination.location!=storage::Location::Overflow && action.destination.location!=storage::Location::Equipped && _native.nextId<UINT64_MAX) {
            const auto next=_native.nextId;
            auto copy=*record; copy.id=next;
            _native.records.reserve(_native.records.size()+1);
            result=_native.grid.split(id,next,action.quantity,action.destination);
            if(result==storage::Result::Ok) { ++_native.nextId; _native.records.push_back(std::move(copy)); }
        } else if(action.kind==ui::ActionKind::Merge) {
            result=_native.grid.merge(id,action.target);
            if(result==storage::Result::Ok) std::erase_if(_native.records,[&](const auto& r){return r.id==id;});
        }
        if(result==storage::Result::Ok) { _layoutDirty=true; (void)reconcileNative(); ++_generation; }
        return result;
    }
    std::optional<storage::Kind> BackpackStorage::kind(storage::ItemId id) const
    {
        std::scoped_lock lock(_mutex);
        const auto* entry=_native.grid.find(id);
        return entry?std::optional{entry->kind}:std::nullopt;
    }
    std::uint32_t BackpackStorage::form(storage::ItemId id) const
    {
        std::scoped_lock lock(_mutex); const auto* record=_native.find(id);
        return record?record->observed.form:0;
    }
    BackpackStorage::Delivery BackpackStorage::withdraw(storage::ItemId id, const RE::NiPoint3& position,std::uint64_t expectedGeneration)
    {
        std::scoped_lock lock(_mutex);
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || _pending.id || _nativeStow || !reconcileNative() || expectedGeneration!=_generation) return {};
        const auto value=nativeRow(id); if(!value.formId) return {};
        const auto handle=native::withdraw(*player,value,1,position);
        const auto ref=handle.get(); if(!ref) return {};
        _pending={kBags,id,ref->formID,ref}; _nativePendingForm=value.formId;
        (void)reconcileNative(id); ++_generation;
        return {handle,id};
    }
    void BackpackStorage::consume(Bag& bag, storage::ItemId id)
    {
        const auto* entry = bag.grid.find(id); if (!entry) return;
        if (entry->count > 1) { bag.grid.setCount(id, entry->count - 1); bag.grid.setBusy(id, false); }
        else {
            if(const auto* record=find(bag,id); record && record->reference) {
                const auto ref=record->lease?record->lease:resolve(record->reference);
                const auto backing=container(bag);
                if(ref && backing && !native::releaseStoredReference(*ref,*backing))
                    logger::error("Backpack persistence owner could not retire after delivery: ref={:08X} owner={:08X}",record->reference,backing->formID);
            }
            bag.grid.erase(id); std::erase_if(bag.records, [id](const auto& record) { return record.id == id; });
        }
    }
    bool BackpackStorage::restorePending()
    {
        if (!_pending.id || _pending.bag >= kBags) return true;
        auto& bag = _bags[_pending.bag]; auto* record = find(bag, _pending.id);
        if (!_pending.lease) _pending.lease = resolve(_pending.reference);
        const auto ref = _pending.lease;
        if (!record || !ref) return false;
        const auto* base = ref->GetObjectReference();
        if (ref->GetFormType() != RE::ENUM_FORM_ID::kREFR || !base || base->formID != record->form ||
            (record->reference && (!weaponReference(*record) || ref->formID != record->reference))) return false;
        if (record->reference) { if (!native::park(*ref)) return false; }
        else {
            auto backing = container(bag); if (!backing) return false;
            _transferIdentity = key(native::ensureIdentity(*ref));
            if (!native::deposit(*backing, *ref)) return false;
            const auto restored = identity(_transferIdentity.load());
            // A whole-stack withdrawal can acquire a new native unique ID on
            // rollback. Logical splits still share their original live stack.
            if (bag.grid.find(record->id)->count == 1) {
                record->stack = restored; bag.grid.setStackKey(record->id, key(restored));
            }
        }
        bag.grid.setBusy(record->id, false); return true;
    }
    bool BackpackStorage::completeDelivery(bool success)
    {
        std::scoped_lock lock(_mutex);
        if(!_pending.id) return true;
        if(_pending.bag<kBags) {
            if(success) consume(_bags[_pending.bag],_pending.id);
            else if(!restorePending()) { _loadedPending=true; return false; }
        } else if(!success) {
            auto* player=RE::PlayerCharacter::GetSingleton();
            if(!_pending.lease) _pending.lease=resolve(_pending.reference);
            const auto ref=_pending.lease;
            if(!player || !ref || !ref->GetObjectReference() || ref->GetObjectReference()->formID!=_nativePendingForm ||
                !native::deposit(*player,*ref,false)) { _loadedPending=true; return false; }
        }
        _pending={}; _nativePendingForm=0; _loadedPending=false;
        (void)reconcileNative(); ++_generation; return true;
    }
    bool BackpackStorage::use(storage::ItemId id, ui::ActionKind action,std::uint64_t expectedGeneration, player_inventory::Row* transferredWeapon)
    {
        std::scoped_lock lock(_mutex);
        if(_pending.id || _nativeStow || !reconcileNative() || expectedGeneration!=_generation) return false;
        auto value=nativeRow(id); if(!value.formId) return false;
        bool accepted{};
        if(action==ui::ActionKind::Use) accepted=player_inventory::useItem(value)==player_inventory::UseActionResult::Used;
        else if(action==ui::ActionKind::EquipArmor || action==ui::ActionKind::UnequipArmor) {
            const auto result=player_inventory::setArmorEquipped(value,action==ui::ActionKind::EquipArmor);
            accepted=result==player_inventory::ArmorActionResult::Equipped || result==player_inventory::ArmorActionResult::Unequipped || result==player_inventory::ArmorActionResult::AlreadyInState;
        } else if(action==ui::ActionKind::ToggleWeapon && !transferredWeapon) {
            accepted=value.equipped && stowEquipped(value.formId,storage::Location::Interior);
        } else if(action==ui::ActionKind::ToggleWeapon && transferredWeapon) {
            auto* player=RE::PlayerCharacter::GetSingleton();
            if(!player || value.equipped) return false;
            const auto uid=native::pinWeapon(*player,value); if(!uid) return false;
            (void)reconcileNative();
            for(const auto& current:_nativeSnapshot.rows) if(current.formId==value.formId && current.uniqueBase==uid.base && current.uniqueIndex==uid.index) {
                *transferredWeapon=current; return true;
            }
            return false;
        }
        (void)reconcileNative(id); return accepted;
    }
    std::array<InventoryDisplayItem, 3> BackpackStorage::mounts(std::array<storage::Kind,3>& kinds) const
    {
        std::scoped_lock lock(_mutex); std::array<InventoryDisplayItem,3> result{};
        for(unsigned i=0;i<3;++i) {
            const auto value=mounted(storage::Location(i+1)); result[i]=value.reference; kinds[i]=value.kind;
        }
        return result;
    }
    BackpackStorage::Mount BackpackStorage::mounted(storage::Location location) const
    {
        std::scoped_lock lock(_mutex);
        if(!_nativeReady || _active==kBags) return {};
        const auto* entry=_native.grid.at(location);
        return entry && !entry->busy?Mount{displayItem(entry->id),entry->kind}:Mount{};
    }
    void BackpackStorage::save(const F4SE::SerializationInterface* serial) try
    {
        auto& self = get(); std::scoped_lock lock(self._mutex);
        if (self._managedRecordPresent || self._backpackReference) {
            const std::array<std::uint32_t,2> managed{self._backpackReference,self._backpackBase};
            if (!serial->WriteRecord(managedRecord,1,managed.data(),sizeof(managed))) logger::error("Managed backpack save failed");
        }
        for (unsigned i = 0; i < kBags; ++i) if (const auto id = self._identities[i].load()) {
            const auto& bag = self._bags[i];
            if (bag.records.size() > (UINT32_MAX-sizeof(SavedBag))/sizeof(SavedItem)) {
                logger::error("Backpack inventory exceeds the native co-save record size"); return;
            }
            SavedBag header{id, bag.container, bag.reference, bag.grid.columns(), bag.grid.rows(), unsigned(bag.records.size())};
            if (!serial->OpenRecord(bagRecord, 2) || !serial->WriteRecordData(&header, sizeof(header))) { logger::error("Backpack co-save header failed"); return; }
            for (const auto& record : bag.records) {
                const auto* item = bag.grid.find(record.id); if (!item) { logger::error("Backpack co-save ledger mismatch"); return; }
                SavedItem saved{record.id, record.appearance, record.form, record.reference, record.stack.base, item->count,
                    item->placement.x, item->placement.y, record.stack.index, std::uint8_t(item->kind), std::uint8_t(item->placement.location), std::uint8_t(item->placement.rotated)};
                if (!serial->WriteRecordData(&saved, sizeof(saved))) { logger::error("Backpack co-save item failed"); return; }
            }
        }
        self.saveNative(serial);
        if (self._pending.id && self._pending.bag<kBags) {
            const std::array<std::uint64_t, 3> pending{self._identities[self._pending.bag].load(), self._pending.id, self._pending.reference};
            if (!serial->WriteRecord(pendingRecord, 2, pending.data(), sizeof(pending))) logger::error("Backpack pending transfer save failed");
        }
    }
    catch (...) { logger::error("Backpack serialization save failed"); }
    void BackpackStorage::revert(const F4SE::SerializationInterface*) try
    {
        auto& self = get(); std::scoped_lock lock(self._mutex);
        for (auto& id : self._identities) id = 0;
        for (auto& bag : self._bags) bag = {};
        self._nativeAccess=false; self._nativeRowsById.clear(); self._legacyImported=false; self._layoutDirty=false; self._nativeDirty=true; self._nextNativeProbe={}; self._nativeStow.reset(); self._nativeStowResult.reset(); self._native=inventory_mirror::Ledger(); self._nativeSnapshot={}; self._nativeReady=false; self._nativePendingForm=0;
        self._pending = {}; self._active = kBags; self._nextId = 1; ++self._generation;
        self._backpackReference = self._backpackBase = 0; self._backpackLease.reset(); self._managedRecordPresent = false;
        self._loadedPending = false; self._nextWeightUpdate = {}; backpack_weight::publish(0);
    }
    catch (...) { logger::error("Backpack serialization reset failed"); }
    void BackpackStorage::load(const F4SE::SerializationInterface* serial) try
    {
        revert(serial); auto& self = get(); std::scoped_lock lock(self._mutex);
        const auto remap = [&](std::uint32_t id) { return id ? serial->ResolveFormID(id).value_or(0) : 0; };
        std::array<std::uint64_t, 3> pending{};
        std::uint32_t type{}, version{}, bytes{}; unsigned count{};
        for (unsigned n = 0; n < kBags*2+8 && serial->GetNextRecordInfo(type, version, bytes); ++n) {
            if(self.loadNative(serial,type,version,bytes)) continue;
            if (type == managedRecord && version == 1 && bytes == 8) {
                std::array<std::uint32_t,2> managed{};
                if (serial->ReadRecordData(managed.data(),sizeof(managed)) == sizeof(managed)) {
                    self._backpackReference = remap(managed[0]); self._backpackBase = remap(managed[1]);
                    self._managedRecordPresent = true;
                    if (!self._backpackReference) logger::error("Saved managed backpack reference {:08X} cannot be resolved; existing ledgers retained, replacement refused",managed[0]);
                }
                continue;
            }
            // The old test inventories are intentionally retired. The managed
            // backpack reference is independent and survives this format change.
            if (version != 2) continue;
            if (type == pendingRecord && bytes == sizeof(pending)) { serial->ReadRecordData(pending.data(), sizeof(pending)); continue; }
            if (type != bagRecord || count == kBags || bytes < sizeof(SavedBag)) continue;
            SavedBag header;
            if (serial->ReadRecordData(&header, sizeof(header)) != sizeof(header) || header.policy != 2 ||
                bytes != sizeof(header) + std::uint64_t(header.count) * sizeof(SavedItem) || !header.columns || !header.rows || header.columns > 32 || header.rows > 32) continue;
            auto uid = identity(header.identity); uid.base = remap(uid.base);
            if (!uid) continue;
            Bag bag; bag.container = remap(header.container); bag.reference = remap(header.reference);
            if (!bag.container) continue;
            bool duplicate{};
            for (unsigned prior = 0; prior < count; ++prior)
                duplicate |= self._identities[prior].load() == key(uid) || self._bags[prior].container == bag.container;
            if (duplicate) { logger::error("Backpack co-save duplicated a bag identity or container"); continue; }
            bag.grid = storage::Grid(header.columns, header.rows); bool valid = true;
            for (unsigned j = 0; j < header.count; ++j) {
                SavedItem saved;
                if (serial->ReadRecordData(&saved, sizeof(saved)) != sizeof(saved) || saved.rotated > 1 || !saved.id || saved.id == UINT64_MAX || !saved.appearance || saved.appearance > saved.id) { valid = false; break; }
                Record record{saved.id, remap(saved.form), remap(saved.reference), {remap(saved.stackBase), saved.stackIndex}};
                record.appearance = saved.appearance;
                if (saved.reserved[0] || saved.reserved[1] || saved.reserved[2] ||
                    (saved.reference && !storage::isWeapon(storage::Kind(saved.kind)))) { valid = false; break; }
                for (unsigned prior = 0; prior < count; ++prior) for (const auto& item : self._bags[prior].records)
                    if (item.id == record.id || (record.reference && item.reference == record.reference)) valid = false;
                for (const auto& item : bag.records) if (record.reference && item.reference == record.reference) valid = false;
                if (!valid) break;
                storage::Entry entry{record.id, storage::Kind(saved.kind), saved.quantity, saved.reference ? 0 : key(record.stack),
                    {storage::savedLocation(saved.location), saved.x, saved.y, saved.rotated != 0}};
                if (bag.grid.insert(entry) != storage::Result::Ok) { valid = false; break; }
                bag.records.push_back(std::move(record)); self._nextId = (std::max)(self._nextId, saved.id + 1);
            }
            if (!valid) { logger::error("Backpack save contains an invalid layout; native container {:08X} preserved", bag.container); continue; }
            self._bags[count] = std::move(bag); self._identities[count++] = key(uid);
        }
        if (pending[1]) {
            auto uid = identity(pending[0]); uid.base = remap(uid.base);
            for (unsigned i = 0; i < count; ++i) if (self._identities[i].load() == key(uid) && self._bags[i].grid.find(pending[1])) {
                self._pending = {i, pending[1], remap(std::uint32_t(pending[2]))}; self._loadedPending = true;
                self._bags[i].grid.setBusy(pending[1], true); break;
            }
        }
        logger::info("Backpack co-save restored {} bag inventories", count);
    }
    catch (...) { logger::error("Backpack serialization load failed; unprocessed native contents preserved"); }
}
