#pragma once
#include "InventoryCapacity.h"
#include <string>
#include <unordered_map>
#include <utility>

namespace rock_backpack::inventory_mirror
{
    struct Stack {
        std::uint32_t form{},index{},count{},uniqueBase{};
        std::uint16_t uniqueIndex{};
        storage::Kind kind{storage::Kind::Misc};
        std::string name;
        float weight{};
        bool equipped{};
        std::uint64_t stackWitness{},instanceWitness{}; // comparisons only; never serialized or dereferenced
    };
    struct Record {
        storage::ItemId id{};
        std::uint64_t appearance{};
        Stack observed;
    };
    inline std::uint64_t nativeKey(const Stack& s)
    { return s.uniqueBase && s.uniqueIndex?(std::uint64_t(s.uniqueBase)<<16)|s.uniqueIndex:0; }
    inline bool sameDescription(const Stack& a,const Stack& b)
    { return a.form==b.form && a.kind==b.kind && a.name==b.name && a.weight==b.weight; }

    // Presentation metadata only. The caller supplies a fresh authoritative
    // native inventory snapshot; reconciliation never creates/deletes items.
    class Ledger {
    public:
        storage::Grid grid;
        std::vector<Record> records;
        storage::ItemId nextId{1};
        std::uint64_t nextAppearance{1};

        const Record* find(storage::ItemId id) const {
            const auto it=std::find_if(records.begin(),records.end(),[&](const auto& r){return r.id==id;});
            return it==records.end()?nullptr:&*it;
        }
        Record* find(storage::ItemId id) { return const_cast<Record*>(std::as_const(*this).find(id)); }

        bool reconcile(std::span<const Stack> native,storage::ItemId preferredDebit=0) {
            std::unordered_multimap<std::uint64_t,std::size_t> identities, witnesses;
            std::unordered_multimap<std::uint32_t,std::size_t> forms;
            for(std::size_t i=0;i<native.size();++i) {
                const auto& s=native[i]; if(!s.form || !s.count || !storage::validKind(s.kind)) return false;
                if(const auto uid=nativeKey(s)) identities.emplace(uid,i);
                if(s.stackWitness) witnesses.emplace(s.stackWitness,i);
                forms.emplace(s.form,i);
            }
            const auto match=[&](const Record& record)->std::optional<std::size_t> {
                const auto& old=record.observed;
                const auto unique=[&](const auto& index,auto key,auto predicate)->std::optional<std::size_t> {
                    std::optional<std::size_t> result;
                    const auto [first,last]=index.equal_range(key);
                    for(auto it=first;it!=last;++it) if(predicate(native[it->second])) {
                        if(result) return {}; result=it->second;
                    }
                    return result;
                };
                if(const auto uid=nativeKey(old))
                    return unique(identities,uid,[&](const Stack& s){return s.form==old.form;});
                if(old.stackWitness) if(const auto live=unique(witnesses,old.stackWitness,[&](const Stack& s){return s.form==old.form && !nativeKey(s);})) return live;
                // Only reuse a layout for an unambiguous unpinned stack after
                // native restacking/load. Ambiguous variants get fresh layouts;
                // their actual native instances and quantities remain intact.
                return unique(forms,old.form,[&](const Stack& s){return !nativeKey(s) && sameDescription(old,s);});
            };
            storage::Grid next(grid.columns(),grid.rows());
            if(const auto& reservation=grid.reservation();reservation && !next.reserveInterior(reservation->kind,reservation->placement)) return false;
            std::vector<Record> kept; kept.reserve(records.size()+native.size());
            std::vector<std::uint32_t> remaining; remaining.reserve(native.size());
            for(const auto& s:native) remaining.push_back(s.count);
            std::vector<std::size_t> order; order.reserve(records.size());
            for(std::size_t i=0;i<records.size();++i) order.push_back(i);
            std::vector<std::optional<std::size_t>> matched; matched.reserve(records.size());
            for(const auto& record:records) matched.push_back(match(record));
            const auto rank=[&](std::size_t i) {
                const auto* e=grid.find(records[i].id);
                const bool debited=records[i].id==preferredDebit && matched[i] && native[*matched[i]].count<records[i].observed.count;
                return debited?4:e && storage::isMount(e->placement.location)?
                    (matched[i] && native[*matched[i]].equipped?2:0):
                    e && e->placement.location==storage::Location::Overflow?3:
                    e && e->placement.location==storage::Location::Equipped?2:1;
            };
            std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return rank(a)<rank(b);});
            std::vector<std::optional<std::size_t>> mergeTarget(native.size());
            std::vector<std::uint64_t> groupKey(native.size());
            auto idCounter=nextId,appearanceCounter=nextAppearance;
            // Native equipped flags apply to a stack. Exactly ONE weapon unit
            // is in the hand; any other units still require interior capacity.
            std::vector<bool> consumed(records.size());
            for(std::size_t n=0;n<native.size();++n) {
                const auto& source=native[n];
                if(!source.equipped || !storage::isWeapon(source.kind)) continue;
                std::optional<std::size_t> reuse;
                for(const auto i:order) if(matched[i]==n) {
                    const auto* e=grid.find(records[i].id);
                    if(e && e->placement.location==storage::Location::Equipped) { reuse=i; break; }
                    if(!reuse && e && e->count==1) reuse=i;
                }
                if(!idCounter || idCounter==UINT64_MAX || !appearanceCounter || appearanceCounter==UINT64_MAX) return false;
                Record record=reuse?records[*reuse]:Record{idCounter++,appearanceCounter++,source};
                if(reuse) {
                    consumed[*reuse]=true;
                    if(!sameDescription(record.observed,source) || record.observed.instanceWitness!=source.instanceWitness)
                        record.appearance=appearanceCounter++;
                    record.observed=source;
                }
                groupKey[n]=record.id;
                if(next.insert({record.id,source.kind,1,record.id,{storage::Location::Equipped}})!=storage::Result::Ok) return false;
                kept.push_back(std::move(record)); --remaining[n];
            }
            for(const auto i:order) {
                if(consumed[i]) continue;
                const auto* entry=grid.find(records[i].id); if(!entry) return false;
                const auto index=matched[i]; if(!index || !remaining[*index]) continue;
                const auto& source=native[*index];
                auto copy=*entry; copy.kind=source.kind;
                copy.count=(std::min)(copy.count,remaining[*index]);
                if(!groupKey[*index]) groupKey[*index]=copy.id;
                copy.stackKey=groupKey[*index];
                const bool relocate=copy.placement.location==storage::Location::Overflow || copy.placement.location==storage::Location::Equipped ||
                    (storage::isMount(copy.placement.location) && copy.count!=1);
                if(relocate || next.fits(copy.kind,copy.placement)!=storage::Result::Ok) {
                    const auto admission=inventory_capacity::evaluate(next,copy.kind,false,inventory_capacity::Source::ExistingInventory);
                    if(!admission.accepted || !admission.placement) return false;
                    copy.placement=*admission.placement;
                }
                if(next.insert(copy)!=storage::Result::Ok) return false;
                auto record=records[i];
                if(!sameDescription(record.observed,source) || record.observed.instanceWitness!=source.instanceWitness) {
                    if(!appearanceCounter || appearanceCounter==UINT64_MAX) return false;
                    record.appearance=appearanceCounter++;
                }
                record.observed=source;
                remaining[*index]-=copy.count;
                if(!storage::isMount(copy.placement.location) &&
                    (!mergeTarget[*index] || copy.placement.location!=storage::Location::Overflow)) mergeTarget[*index]=kept.size();
                kept.push_back(std::move(record));
            }
            for(std::size_t i=0;i<native.size();++i) if(remaining[i]) {
                if(mergeTarget[i]) {
                    const auto id=kept[*mergeTarget[i]].id;
                    const auto* entry=next.find(id);
                    if(!entry || remaining[i]>UINT32_MAX-entry->count || !next.setCount(id,entry->count+remaining[i])) return false;
                } else {
                    if(!idCounter || idCounter==UINT64_MAX || !appearanceCounter || appearanceCounter==UINT64_MAX) return false;
                    const auto& s=native[i];
                    const auto admission=inventory_capacity::evaluate(next,s.kind,false,inventory_capacity::Source::ExistingInventory);
                    if(!groupKey[i]) groupKey[i]=idCounter;
                    if(!admission.placement || next.insert({idCounter,s.kind,remaining[i],groupKey[i],*admission.placement})!=storage::Result::Ok) return false;
                    kept.push_back({idCounter++,appearanceCounter++,s});
                }
            }
            grid=std::move(next); records=std::move(kept); nextId=idCounter; nextAppearance=appearanceCounter;
            return true;
        }
    };
}
