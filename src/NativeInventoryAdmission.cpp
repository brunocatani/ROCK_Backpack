#include "PCH.h"
#include "NativeInventoryAdmission.h"
#include "BackpackStorage.h"
#include <F4SE/Trampoline.h>
#include <RE/Bethesda/BGSInventoryInterface.h>
#include <cstring>
#include <intrin.h>
#include <RPS/Runtime/HookChain.h>

namespace rock_backpack::native_admission
{
    namespace {
        // VR uses ushort stack indexes here. The inherited CommonLib menu
        // declaration uses uint8 and must not be used for this native record.
        struct Entry { std::uint32_t handle{},reserved{}; RE::BSTSmallArray<std::uint16_t,4> stacks; };
        static_assert(sizeof(Entry)==0x20 && offsetof(Entry,stacks)==8);
        using Pickup=void(*)(RE::PlayerCharacter*,RE::TESObjectREFR*,std::uint32_t,bool);
        using Transfer=std::uint32_t(*)(void*,const Entry*,std::uint32_t,bool);
        Pickup originalPickup{};
        bool installed{};
        std::atomic<const rock::api::weapon::Api*> rockWeapon{};
        std::atomic<rock::api::OwnerToken> rockOwner{};
        [[nodiscard]] bool equipPickup(std::uint32_t reference,std::uint32_t count) noexcept
        {
            const auto* weapon=rockWeapon.load(std::memory_order_acquire);
            const auto owner=rockOwner.load(std::memory_order_acquire);
            std::uint32_t matches{};
            return weapon && owner && weapon->isEquipPickup(owner,reference,count,&matches)==rock::api::Status::Ok && matches;
        }
        std::atomic_uint64_t nextMessage{};
        // Scope only the synchronous engine transfer we have already admitted.
        // Its world-reference substep must not refuse after the source-removal
        // path has committed to continuing. No scope crosses a queued task.
        thread_local unsigned admittedTransfer{};
        struct Admitted { Admitted() { ++admittedTransfer; } ~Admitted() { --admittedTransfer; } };
        template<class F> F function(std::uintptr_t rva) { return reinterpret_cast<F>(REL::Module::get().base()+rva); }
        // Each patched call and what it reached: the native function or another
        // plugin's detour. Written once at install before any site is patched.
        struct Site { std::uintptr_t address{},callee{}; };
        std::array<Site,6> sites{};
        template<class F> F calleeFor(void* returnAddress,std::uintptr_t nativeRva)
        {
            const auto at=reinterpret_cast<std::uintptr_t>(returnAddress)-5;
            for(const auto& site:sites) if(site.address==at && site.callee) return reinterpret_cast<F>(site.callee);
            return function<F>(nativeRva);
        }
        void pickup(RE::PlayerCharacter* player,RE::TESObjectREFR* reference,std::uint32_t count,bool sound) noexcept
        {
            try {
                if(reference && !admittedTransfer && BackpackStorage::get().policyActive() &&
                    !equipPickup(reference->GetFormID(),count)) {
                    const auto row=player_inventory::describe(*reference);
                    if(row.formId && !BackpackStorage::get().admitNative(row,reference->extraList.get())) { refused(); return; }
                }
                originalPickup(player,reference,count,sound);
            } catch(...) { logger::error("Native world pickup admission failed; acquisition withheld"); refused(); }
        }
        std::uint32_t transfer(void* menu,const Entry* entry,std::uint32_t count,bool taking) noexcept
        {
            const auto original=calleeFor<Transfer>(_ReturnAddress(),0x12E5360);
            if(!taking || !BackpackStorage::get().policyActive()) return original(menu,entry,count,taking);
            try {
                if(!menu || !entry || entry->stacks.empty() || entry->stacks.size()>65536) { refused(); return 1; }
                // Independently verified in 12E5360 and its native callers:
                // menu+448 owns the source handle; entry+0 indexes the global
                // inventory inventoryService, with ushort stack IDs at +8.
                const auto sourceHandle=*reinterpret_cast<const RE::ObjectRefHandle*>(static_cast<const std::byte*>(menu)+0x448);
                const auto source=sourceHandle.get(); if(!source) return 1;
                const auto inventoryInterface=*reinterpret_cast<void**>(REL::Module::get().base()+0x5936A60);
                const auto* native=function<const RE::BGSInventoryItem*(*)(void*,const std::uint32_t*)>(0x1AB6B0)(inventoryInterface,&entry->handle);
                if(!native || !native->object) return 1;
                const auto form=native->object->formID;
                const auto snapshot=player_inventory::capture(*source);
                if(!snapshot.available) { refused(); return 1; }
                struct Selected { player_inventory::Row row; std::uint32_t quantity; };
                std::vector<Selected> selected;
                auto remaining=count;
                for(const auto index:entry->stacks) {
                    if(!remaining) break;
                    const auto found=std::find_if(snapshot.rows.begin(),snapshot.rows.end(),[&](const auto& row){return row.formId==form && row.stackId==index;});
                    if(found==snapshot.rows.end()) continue;
                    if(std::any_of(selected.begin(),selected.end(),[&](const auto& value){return value.row.stackId==index;})) continue;
                    const auto quantity=(std::min)(remaining,found->count);
                    selected.push_back({*found,quantity}); remaining-=quantity;
                }
                // Non-presentable engine bookkeeping objects retain native
                // behavior; they are not backpack inventory entries.
                if(selected.empty()) return original(menu,entry,count,taking);
                std::sort(selected.begin(),selected.end(),[](const auto& a,const auto& b){return a.row.stackId>b.row.stackId;});
                std::uint32_t result=1; bool rejected{};
                for(const auto& selectedItem:selected) {
                    const auto& row=selectedItem.row;
                    if(!native_inventory::current(*source,row)) { rejected=true; continue; }
                    const auto extra=native_inventory::extra(*source,row);
                    if(!BackpackStorage::get().admitNative(row,extra.get())) { rejected=true; continue; }
                    Entry one; one.handle=entry->handle; one.reserved=entry->reserved;
                    one.stacks.push_back(static_cast<std::uint16_t>(row.stackId));
                    { Admitted admitted; result=original(menu,&one,selectedItem.quantity,true); }
                    if(result==0 || result==2) break; // native cancellation/pickpocket failure
                }
                if(rejected) refused(count==UINT32_MAX);
                // Native Take All stops on 0/2. A capacity refusal is 1 so it
                // continues to exempt/compatible later items in the source.
                return result;
            } catch(...) { logger::error("Native container admission failed; remaining acquisition withheld"); refused(true); return 1; }
        }
        void quickTake(void* model) noexcept
        {
            const auto original=calleeFor<void(*)(void*)>(_ReturnAddress(),0xA951D0);
            if(!BackpackStorage::get().policyActive()) { original(model); return; }
            try {
                if(!model) return;
                const auto* data=static_cast<const std::byte*>(model);
                const auto source=reinterpret_cast<const RE::ObjectRefHandle*>(data+0xD0)->get();
                const auto size=*reinterpret_cast<const std::uint32_t*>(data+0x2F8);
                const auto index=*reinterpret_cast<const std::int32_t*>(data+0x34C);
                if(!source || index<0 || unsigned(index)>=size || data[0x300]!=std::byte{}) return;
                const auto* entries=*reinterpret_cast<const Entry* const*>(data+0x2E8);
                if(!entries) return;
                const auto& entry=entries[index];
                if(entry.stacks.empty() || entry.stacks.size()>65536) return;
                const auto inventoryInterface=*reinterpret_cast<void**>(REL::Module::get().base()+0x5936A60);
                const auto* native=function<const RE::BGSInventoryItem*(*)(void*,const std::uint32_t*)>(0x1AB6B0)(inventoryInterface,&entry.handle);
                if(!native || !native->object) return;
                const auto form=native->object->formID;
                const auto snapshot=player_inventory::capture(*source);
                if(!snapshot.available) { refused(); return; }
                std::vector<player_inventory::Row> selected;
                for(const auto& row:snapshot.rows) if(row.formId==form && std::find(entry.stacks.begin(),entry.stacks.end(),row.stackId)!=entry.stacks.end())
                    selected.push_back(row);
                if(!BackpackStorage::get().admitNativeBatch(*source,selected)) { refused(); return; }
                Admitted admitted; original(model);
            } catch(...) { logger::error("Quick-container acquisition withheld after admission failure"); refused(); }
        }
        RE::NiPointer<RE::TESObjectREFR> entryOwner(std::uint32_t handle)
        {
            auto* inventoryService=*reinterpret_cast<RE::BGSInventoryInterface**>(REL::Module::get().base()+0x5936A60);
            if(!inventoryService) return {};
            RE::ObjectRefHandle owner;
            // Same agent-array critical section used by native 1AB6B0. Copy
            // the numeric owner handle, then release before inventory reads.
            auto* gate=reinterpret_cast<CRITICAL_SECTION*>(REL::Module::get().base()+0x5936A38);
            {
                struct Guard { CRITICAL_SECTION* gate; explicit Guard(CRITICAL_SECTION* value):gate(value) { EnterCriticalSection(gate); } ~Guard() { LeaveCriticalSection(gate); } } lock(gate);
                const auto& agents=inventoryService->agentArray;
                const auto found=std::lower_bound(agents.begin(),agents.end(),handle,[](const auto& agent,std::uint32_t key){return agent.handleID<key;});
                if(found!=agents.end() && found->handleID==handle) owner=found->itemOwner;
            }
            return owner.get();
        }
        void completeTrade(void* menu) noexcept
        {
            const auto original=calleeFor<void(*)(void*)>(_ReturnAddress(),0xB18840);
            if(!BackpackStorage::get().policyActive()) { original(menu); return; }
            try {
                // CompleteTrade B1892D/B18956 and DoItemTransfer B19A00 agree
                // on the map: capacity +45C, entries +478, 24-byte buckets.
                // Each value's native quantity map has 16-byte buckets and
                // signed quantities (B21F30/B1BE10): positive buys, negative sells.
                using Quantities=RE::BSTHashMap<std::uint32_t,std::int32_t>;
                struct TradeData { Quantities quantities; std::uint32_t caps{}; };
                using Basket=RE::BSTHashMap<std::uint32_t,TradeData*>;
                static_assert(sizeof(TradeData)==0x38 && sizeof(Basket)==0x30);
                if(!menu) return;
                const auto* bytes=static_cast<const std::byte*>(menu);
                if(*reinterpret_cast<const std::uint32_t*>(bytes+0x45C)>1048576) { refused(); return; }
                const auto& basket=*reinterpret_cast<const Basket*>(bytes+0x450);
                std::vector<BackpackStorage::NativeOutgoing> outgoing;
                std::vector<BackpackStorage::NativeIncoming> incoming;
                for(const auto& [handle,data]:basket) {
                    if(!data) continue;
                    const auto source=entryOwner(handle); if(!source) { refused(); return; }
                    const auto inventoryService=*reinterpret_cast<void**>(REL::Module::get().base()+0x5936A60);
                    const auto* item=function<const RE::BGSInventoryItem*(*)(void*,const std::uint32_t*)>(0x1AB6B0)(inventoryService,&handle);
                    if(!item || !item->object) { refused(); return; }
                    const auto form=item->object->formID;
                    const auto snapshot=player_inventory::capture(*source);
                    if(!snapshot.available || *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(data)+0xC)>65536) { refused(); return; }
                    for(const auto& [index,quantity]:data->quantities) {
                        if(!quantity) continue;
                        const auto found=std::find_if(snapshot.rows.begin(),snapshot.rows.end(),[&](const auto& row){return row.formId==form && row.stackId==index;});
                        if(found==snapshot.rows.end()) { refused(); return; }
                        const auto units=quantity<0?std::uint64_t(-std::int64_t(quantity)):std::uint64_t(quantity);
                        if(units>found->count) { refused(); return; }
                        if(quantity<0) {
                            if(source.get()!=RE::PlayerCharacter::GetSingleton()) { refused(); return; }
                            outgoing.push_back({*found,std::uint32_t(units)});
                        } else {
                            auto row=*found; auto extra=native_inventory::extra(*source,row); row.count=std::uint32_t(units);
                            incoming.push_back({std::move(row),std::move(extra)});
                        }
                    }
                }
                if(!BackpackStorage::get().admitTrade(outgoing,incoming)) { refused(); return; }
                Admitted admitted; original(menu);
            } catch(...) { logger::error("Trade admission failed before item/caps transfer; trade withheld"); refused(); }
        }
    }
    void refused(bool bulk,bool stowing) noexcept
    {
        const auto now=GetTickCount64(); auto due=nextMessage.load();
        if(now<due || !nextMessage.compare_exchange_strong(due,now+1500)) return;
        // AFBE30 is the same HUD notification entry called by native container
        // refusals in 12E5360; it copies the message for the HUD.
        function<void(*)(const char*,std::uint32_t,bool,bool)>(0xAFBE30)(
            stowing?"Backpack full - cannot store this weapon":bulk?"Backpack full - some items were left behind":"Backpack full - cannot take this item",0,true,true);
    }
    void bindRock(const rock::api::weapon::Api* weapon,rock::api::OwnerToken owner) noexcept
    {
        rockOwner.store(owner,std::memory_order_release);
        rockWeapon.store(weapon,std::memory_order_release);
    }
    void unbindRock() noexcept
    {
        rockWeapon.store(nullptr,std::memory_order_release);
        rockOwner.store(0,std::memory_order_release);
    }
    bool install()
    {
        if(installed) return true;
        const auto base=REL::Module::get().base();
        enum class Kind { Container,Quick,Trade };
        struct Call { std::uintptr_t rva; std::uintptr_t target; Kind kind{}; };
        constexpr std::array calls{
            Call{0x12E6A48,0x12E5360},
            Call{0x12E7F57,0x12E5360},
            Call{0x12E8392,0x12E5360},
            Call{0xA94C42,0xA951D0,Kind::Quick},
            Call{0xB21361,0xB18840,Kind::Trade},
            Call{0xB21591,0xB18840,Kind::Trade}};
        static_assert(calls.size()==sites.size());
        // The pickup slot and each call reach the native function or another
        // plugin's detour; the hooks call through whichever is there.
        const auto slot=base+0x2D816F0;
        const auto pickupResolved=RPS::Runtime::HookChain::resolveSlot(slot,base+0xF11950);
        if(!pickupResolved) {
            logger::error("Native world pickup slot holds unrecognised code; admission was not installed"); return false;
        }
        if(pickupResolved.foreign()) logger::info("Native world pickup already replaced by {}; admission calls through it",pickupResolved.owner);
        std::array<std::array<std::uint8_t,5>,calls.size()> before{};
        for(std::size_t index=0;index<calls.size();++index) {
            const auto& call=calls[index];
            const auto resolved=RPS::Runtime::HookChain::resolveCall(base+call.rva,base+call.target);
            if(!resolved) {
                logger::error("Native container transfer call differs at {:X}; admission was not installed",call.rva); return false;
            }
            if(resolved.foreign()) logger::info("Native transfer call at {:X} already hooked by {}; admission calls through it",call.rva,resolved.owner);
            sites[index]={base+call.rva,resolved.callee};
            std::memcpy(before[index].data(),reinterpret_cast<const void*>(base+call.rva),5);
        }
        constexpr std::array<std::uint8_t,8> lookup{0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x20};
        constexpr std::array<std::uint8_t,6> notification{0x40,0x53,0x48,0x83,0xEC,0x50};
        if(!RPS::Runtime::HookChain::resolveEntry(base+0x1AB6B0,lookup) ||
            !RPS::Runtime::HookChain::resolveEntry(base+0xAFBE30,notification)) return false;
        unsigned written{};
        try {
            static F4SE::Trampoline trampoline; trampoline.create(128);
            originalPickup=reinterpret_cast<Pickup>(pickupResolved.callee);
            for(const auto& call:calls) { ++written; trampoline.write_call<5>(base+call.rva,
                call.kind==Kind::Quick?reinterpret_cast<std::uintptr_t>(&quickTake):
                call.kind==Kind::Trade?reinterpret_cast<std::uintptr_t>(&completeTrade):reinterpret_cast<std::uintptr_t>(&transfer)); }
            REL::safe_write(slot,reinterpret_cast<std::uintptr_t>(&pickup));
            installed=true;
            logger::info("Native capacity admission installed: world Take, quick-container, container transfer and per-entry Take All; scripted AddItem unchanged");
            return true;
        } catch(...) {
            while(written) { --written; REL::safe_write(base+calls[written].rva,before[written].data(),before[written].size()); }
            REL::safe_write(slot,pickupResolved.callee); originalPickup=nullptr; return false;
        }
    }
}
