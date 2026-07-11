#include "PCH.h"
#include "BackpackStorage.h"
#include "NativeInventoryAdmission.h"
#include <RE/Bethesda/BGSInventoryInterface.h>

namespace rock_backpack
{
    bool BackpackStorage::nativeStackHasSpace(const player_inventory::Row& row) const
    {
        for(const auto& record:_native.records) if(record.observed.form==row.formId && record.observed.stackWitness==row.stackIdentity)
            if(const auto* entry=_native.grid.find(record.id); entry && entry->placement.location==storage::Location::Interior) return true;
        return false;
    }
    namespace {
        class InventoryInvalidation final:public RE::BSTEventSink<RE::InventoryInterface::CountChangedEvent> {
            RE::BSEventNotifyControl ProcessEvent(const RE::InventoryInterface::CountChangedEvent& event,
                RE::BSTEventSource<RE::InventoryInterface::CountChangedEvent>*) override {
                if(event.inventoryOwnerID==0x14) BackpackStorage::get().invalidateNative();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }
    bool BackpackStorage::attachNativeMirrorEvents()
    {
        if(_mirrorEventsRegistered) return true;
        auto* source=*reinterpret_cast<RE::BGSInventoryInterface**>(REL::Module::get().base()+0x5936A60);
        if(!source) return false;
        // The native inventory-interface singleton has process lifetime. This
        // sink owns no player/list pointers, including across save transitions.
        static InventoryInvalidation sink;
        reinterpret_cast<RE::BSTEventSource<RE::InventoryInterface::CountChangedEvent>*>(
            reinterpret_cast<std::byte*>(source)+0x8)->RegisterSink(&sink);
        _mirrorEventsRegistered=true; return true;
    }
    bool BackpackStorage::admitNative(const player_inventory::Row& value,RE::ExtraDataList* source)
    {
        if(!_policyActive.load()) return true;
        std::scoped_lock lock(_mutex);
        if(value.kind==storage::Kind::Misc && value.formId) return true;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || !value.formId || !reconcileNative()) return false;
        bool merge{};
        for(const auto& row:_nativeSnapshot.rows) if(row.formId==value.formId && nativeStackHasSpace(row)) {
            const auto extra=native_inventory::extra(*player,row);
            if(native_inventory::compatible(extra.get(),source)) { merge=true; break; }
        }
        return inventory_capacity::evaluate(_native.grid,value.kind,merge,inventory_capacity::Source::Voluntary).accepted;
    }
    bool BackpackStorage::admitNativeBatch(RE::TESObjectREFR& source,std::span<const player_inventory::Row> values)
    {
        std::vector<NativeIncoming> incoming; incoming.reserve(values.size());
        for(const auto& value:values) {
            if(!native_inventory::current(source,value)) return false;
            incoming.push_back({value,native_inventory::extra(source,value)});
        }
        return admitTrade({},incoming);
    }
    bool BackpackStorage::admitTrade(std::span<const NativeOutgoing> outgoing,std::span<const NativeIncoming> incoming)
    {
        if(!_policyActive.load()) return true;
        std::scoped_lock lock(_mutex);
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || !reconcileNative()) return false;
        std::vector<inventory_mirror::Stack> projected;
        for(const auto& row:_nativeSnapshot.rows) projected.push_back({row.formId,row.stackId,row.count,
            row.uniqueBase,row.uniqueIndex,row.kind,row.name,row.weight,row.equipped,row.stackIdentity,row.instanceIdentity});
        for(const auto& sale:outgoing) {
            const auto found=std::find_if(projected.begin(),projected.end(),[&](const auto& row){
                return row.form==sale.row.formId && row.stackWitness==sale.row.stackIdentity && row.instanceWitness==sale.row.instanceIdentity;
            });
            if(found==projected.end() || sale.quantity>found->count) return false;
            found->count-=sale.quantity;
        }
        std::erase_if(projected,[](const auto& row){return !row.count;});
        auto proposal=_native;
        if(!proposal.reconcile(projected)) return false;
        auto& reserved=proposal.grid; auto next=proposal.nextId;
        std::vector<const NativeIncoming*> accepted; accepted.reserve(incoming.size());
        for(const auto& value:incoming) {
            bool merge{};
            for(const auto& row:_nativeSnapshot.rows) if(row.formId==value.row.formId) {
                bool space{};
                for(const auto& record:proposal.records) if(record.observed.form==row.formId && record.observed.stackWitness==row.stackIdentity)
                    if(const auto* entry=reserved.find(record.id); entry && entry->placement.location==storage::Location::Interior) { space=true; break; }
                if(!space) continue;
                const auto target=native_inventory::extra(*player,row);
                if(native_inventory::compatible(target.get(),value.extra.get())) { merge=true; break; }
            }
            if(!merge) for(const auto* row:accepted) if(row->row.formId==value.row.formId && native_inventory::compatible(row->extra.get(),value.extra.get())) { merge=true; break; }
            const auto result=inventory_capacity::evaluate(reserved,value.row.kind,merge,inventory_capacity::Source::Voluntary);
            if(!result.accepted) return false;
            if(!merge && (!next || next==UINT64_MAX || !result.placement ||
                reserved.insert({next++,value.row.kind,value.row.count,0,*result.placement})!=storage::Result::Ok)) return false;
            accepted.push_back(&value);
        }
        return true;
    }
    bool BackpackStorage::reconcileNative(storage::ItemId debit)
    {
        auto snapshot=player_inventory::capture();
        if(!snapshot.available) { _nativeReady=false; return false; }
        if(_native.grid.reservation()) {
            bool stillEquipped{};
            if(_nativeStow) for(const auto& row:snapshot.rows) {
                const auto& expected=_nativeStow->row;
                const bool same=expected.uniqueBase && expected.uniqueIndex?
                    row.uniqueBase==expected.uniqueBase && row.uniqueIndex==expected.uniqueIndex:
                    row.stackIdentity==expected.stackIdentity && row.instanceIdentity==expected.instanceIdentity;
                if(row.formId==expected.formId && same && row.equipped) { stillEquipped=true; break; }
            }
            if(!stillEquipped) { _native.grid.clearReservation(); _layoutDirty=true; }
        }
        if(_nativeReady && !_layoutDirty && !debit && snapshot.rows==_nativeSnapshot.rows) return true;
        struct PublishGuard { std::atomic_bool& ready; bool published{}; ~PublishGuard() { if(!published) ready=false; } } publication{_nativeReady};
        std::unordered_map<std::uint64_t,std::size_t> sourceRows;
        sourceRows.reserve(snapshot.rows.size());
        for(std::size_t i=0;i<snapshot.rows.size();++i) sourceRows.emplace(snapshot.rows[i].stackIdentity,i);
        std::vector<inventory_mirror::Stack> stacks; stacks.reserve(snapshot.rows.size());
        for(const auto& row:snapshot.rows) stacks.push_back({row.formId,row.stackId,row.count,
            row.uniqueBase,row.uniqueIndex,row.kind,row.name,row.weight,row.equipped,row.stackIdentity,row.instanceIdentity});
        _native.nextId=(std::max)(_native.nextId,_nextId);
        _native.nextAppearance=(std::max)(_native.nextAppearance,_nextId);
        if(!_native.reconcile(stacks,debit)) {
            _nativeReady=false;
            logger::error("Native backpack inventory could not be reconciled; native items unchanged");
            return false;
        }
        std::unordered_map<storage::ItemId,std::size_t> rowsById; rowsById.reserve(_native.records.size());
        for(const auto& record:_native.records) if(const auto found=sourceRows.find(record.observed.stackWitness);found!=sourceRows.end())
            rowsById.emplace(record.id,found->second);
        _nativeRowsById=std::move(rowsById); _nativeSnapshot=std::move(snapshot);
        _nativeReady=true; _layoutDirty=false; publication.published=true; ++_generation; return true;
    }
    player_inventory::Row BackpackStorage::nativeRow(storage::ItemId id) const
    {
        const auto found=_nativeRowsById.find(id);
        return found!=_nativeRowsById.end() && found->second<_nativeSnapshot.rows.size()?_nativeSnapshot.rows[found->second]:player_inventory::Row{};
    }
    InventoryDisplayItem BackpackStorage::displayItem(storage::ItemId id) const
    {
        const auto* record=_native.find(id); const auto* entry=_native.grid.find(id);
        if(!record || !entry || entry->count!=1 || record->observed.equipped) return {};
        const auto& s=record->observed;
        return {id,record->appearance,s.stackWitness,s.instanceWitness,s.form,s.index,s.count,s.kind};
    }
    bool BackpackStorage::placeNative(storage::ItemId id,storage::Placement destination)
    {
        const auto* entry=_native.grid.find(id); if(!entry || entry->busy || entry->placement.location==storage::Location::Equipped ||
            _native.grid.fits(entry->kind,destination,id)!=storage::Result::Ok) return false;
        if(!storage::isMount(destination.location)) {
            const bool moved=_native.grid.move(id,destination)==storage::Result::Ok;
            _layoutDirty|=moved; return moved;
        }
        auto* player=RE::PlayerCharacter::GetSingleton(); const auto row=nativeRow(id);
        if(!player || !row.formId || row.equipped) return false;
        const auto uid=native_inventory::pinWeapon(*player,row); if(!uid) return false;
        if(!reconcileNative()) return false;
        // Native splitting may create a new stack and change every stack index.
        // Locate only the exact pinned unit after a fresh native snapshot.
        for(const auto& record:_native.records) if(record.observed.form==row.formId &&
            record.observed.uniqueBase==uid.base && record.observed.uniqueIndex==uid.index)
        {
            const bool moved=_native.grid.move(record.id,destination)==storage::Result::Ok;
            _layoutDirty|=moved; return moved;
        }
        return false;
    }
    bool BackpackStorage::stowEquipped(std::uint32_t form,storage::Location location)
    {
        std::scoped_lock lock(_mutex);
        if(_nativeStow || _nativeStowResult || _pending.id || !reconcileNative()) return false;
        const player_inventory::Row* selected{};
        for(const auto& row:_nativeSnapshot.rows) if(row.formId==form && row.equipped) {
            if(selected) return false; selected=&row;
        }
        if(!selected) return false;
        const auto admission=inventory_capacity::forDestination(_native.grid,selected->kind,location);
        if(!admission.accepted) {
            if(location==storage::Location::Interior) native_admission::refused(false,true);
            return false;
        }
        NativeStow request{*selected,location,std::chrono::steady_clock::now()+std::chrono::seconds(3)};
        if(location==storage::Location::Interior) {
            const auto placement=_native.grid.firstFit(selected->kind);
            if(!placement || !_native.grid.reserveInterior(selected->kind,*placement)) return false;
        }
        const auto outcome=player_inventory::toggleWeapon(request.row);
        if(outcome!=player_inventory::WeaponActionResult::Unequipped &&
            outcome!=player_inventory::WeaponActionResult::NativeActionRejected) {
            _native.grid.clearReservation();
            logger::warn("Equipped stash could not target item: form={:08X}, slot={}, reason={}",form,unsigned(location),player_inventory::weaponResultMessage(outcome));
            return false;
        }
        // The native call has run. Completion is the exact inventory item's
        // observed unequip + placement, not the immediate dispatch return bit.
        // A real refusal times out with the original equipped item untouched.
        _nativeStow=std::move(request);
        logger::info("Equipped stash native dispatch: form={:08X}, slot={}, result={}",form,unsigned(location),unsigned(outcome));
        logger::info("Equipped mount stow queued: form={:08X}, slot={}, uid={:08X}:{}",form,unsigned(location),_nativeStow->row.uniqueBase,_nativeStow->row.uniqueIndex);
        (void)reconcileNative(); finishNativeStow(); return true;
    }
    void BackpackStorage::finishNativeStow()
    {
        if(!_nativeStow || !_nativeReady) return;
        const auto& expected=_nativeStow->row;
        for(const auto& record:_native.records) {
            const auto& current=record.observed;
            const bool same=expected.uniqueBase && expected.uniqueIndex?
                current.uniqueBase==expected.uniqueBase && current.uniqueIndex==expected.uniqueIndex:
                current.stackWitness==expected.stackIdentity && current.instanceWitness==expected.instanceIdentity;
            if(current.form!=expected.formId || !same || current.equipped) continue;
            const auto location=_nativeStow->location; const auto id=record.id;
            _nativeStow.reset();
            const auto* entry=_native.grid.find(id);
            _nativeStowResult=location==storage::Location::Interior?
                entry && entry->placement.location==storage::Location::Interior:placeNative(id,{location});
            if(*_nativeStowResult) logger::info("Native mount stow confirmed: item={}, slot={}",id,unsigned(location));
            if(!*_nativeStowResult)
                logger::warn("Native weapon stow placement refused; item remains in player inventory");
            ++_generation; return;
        }
        if(std::chrono::steady_clock::now()>=_nativeStow->deadline) {
            logger::warn("Native weapon stow did not observe its exact unequipped item; inventory preserved");
            _nativeStow.reset(); _nativeStowResult=false; _native.grid.clearReservation(); _layoutDirty=true;
        }
    }
    std::optional<bool> BackpackStorage::takeStowResult()
    { std::scoped_lock lock(_mutex); return std::exchange(_nativeStowResult,{}); }
    bool BackpackStorage::importLegacy()
    {
        if(_legacyImported) return true;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || _active==kBags || _pending.id) return false;
        restoreStoredReferences();
        if(!reconcileNative()) return false;
        bool complete=true;
        for(auto& bag:_bags) {
            const auto owner=container(bag);
            if(!owner) { if(bag.container || !bag.records.empty()) complete=false; continue; }
            // Each successful native move is retired from the old metadata
            // immediately. Missing saved references remain recorded for repair;
            // no new item is ever manufactured from their old base form.
            for(std::size_t i=0;i<bag.records.size();) {
                auto& record=bag.records[i];
                const auto* entry=bag.grid.find(record.id);
                if(!entry) { complete=false; ++i; continue; }
                const auto placement=entry->placement;
                bool moved{};
                if(record.reference) {
                    const auto ref=weaponReference(record);
                    if(ref) {
                        _transferIdentity=(std::uint64_t(record.stack.base)<<16)|record.stack.index;
                        const auto uid=native_inventory::ensureIdentity(*ref);
                        _transferIdentity=(std::uint64_t(uid.base)<<16)|uid.index;
                        if(uid && native_inventory::showAt(*ref,player->GetPosition())) {
                            moved=native_inventory::deposit(*player,*ref);
                            if(moved) (void)native_inventory::releaseStoredReference(*ref,*owner);
                            else if(!ref->IsDeleted()) (void)native_inventory::park(*ref);
                        }
                    }
                } else {
                    const auto value=row(bag,record);
                    _transferIdentity=(std::uint64_t(value.uniqueBase)<<16)|value.uniqueIndex;
                    if(value.formId) moved=native_inventory::transfer(*owner,*player,value,(std::min)(entry->count,value.count));
                }
                if(!moved) { complete=false; ++i; continue; }
                const auto uid=_transferIdentity.load(); const auto form=record.form;
                bag.grid.erase(record.id); bag.records.erase(bag.records.begin()+i);
                if(reconcileNative()) for(const auto& current:_native.records) if(current.observed.form==form &&
                    inventory_mirror::nativeKey(current.observed)==uid) {
                    (void)placeNative(current.id,placement); break;
                }
            }
            // Preserve native contents which were not represented by an older
            // layout. The engine's actual stacks, not co-save quantities, win.
            {
                auto leftovers=player_inventory::capture(*owner);
                if(!leftovers.available) complete=false;
                std::sort(leftovers.rows.begin(),leftovers.rows.end(),[](const auto& a,const auto& b) {
                    return a.formId!=b.formId?a.formId<b.formId:a.stackId>b.stackId;
                });
                for(const auto& value:leftovers.rows) {
                    if(!native_inventory::transfer(*owner,*player,value,value.count)) { complete=false; continue; }
                    if(value.uniqueBase && value.uniqueIndex) std::erase_if(bag.records,[&](const auto& record) {
                        if(record.reference || record.form!=value.formId || record.stack.base!=value.uniqueBase || record.stack.index!=value.uniqueIndex) return false;
                        bag.grid.erase(record.id); return true;
                    });
                }
            }
        }
        (void)reconcileNative();
        if(complete) logger::info("Unified inventory adopted: nativeStacks={} layoutEntries={} overflow={}; native inventory owns all imported items",
            _nativeSnapshot.rows.size(),_native.records.size(),std::count_if(_native.grid.entries().begin(),_native.grid.entries().end(),
                [](const auto& entry){return entry.placement.location==storage::Location::Overflow;}));
        _legacyImported=complete; return complete;
    }
}
