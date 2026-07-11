#include "PCH.h"
#include "WeaponMaterialFix.h"
#include "NativeInventory.h"
#include <RE/Bethesda/BGSInventoryItem.h>
#include <RE/Bethesda/TESDataHandler.h>
#include <RE/Bethesda/PlayerCharacter.h>
#include <RE/Bethesda/TESBoundAnimObjects.h>
#include <cstring>

namespace rock_backpack::native_inventory
{
    namespace
    {
        // FO4VR 1.2.72, unpacked SHA256 95abb321dd5f8e9536665d8ea0a8dc550b8a5c7878516f3a1727569844b0bbf6.
        // Raw caller/callee evidence: PhysicalInventory/references/binary-2026-09-27-storage.
        struct Guard { std::uintptr_t rva; std::array<unsigned char, 12> bytes; };
        constexpr Guard guards[]{
            {0x98830,{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B}},
            {0x98880,{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x41}},
            {0x3E3A10,{0x4C,0x8B,0xDC,0x53,0x56,0x57,0x48,0x83,0xEC,0x60,0x33,0xF6}},
            {0x1B4850,{0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x18,0x49,0x89,0x53,0x10,0x57}},
            {0x1AE3F0,{0x48,0x83,0xEC,0x28,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74}},
            {0x813A0,{0x48,0x83,0xC1,0x08,0xB2,0x01,0xE9,0xC5,0xF9,0xFF,0xFF,0xCC}},
            {0x8BF60,{0x48,0x83,0xEC,0x28,0xB2,0x24,0xE8,0x85,0x83,0xFB,0xFF,0x48}},
            {0x3F6FE0,{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x48,0x83,0xEC,0x20,0x4C}},
            {0x3E15E0,{0x44,0x89,0x44,0x24,0x18,0x55,0x57,0x41,0x57,0x48,0x81,0xEC}},
            {0x3F63C0,{0x4C,0x8B,0xDC,0x41,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00}},
            {0x3F5DA0,{0x88,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x56,0x48,0x83}},
            {0x4E6E00,{0x48,0x8B,0xC4,0x55,0x56,0x57,0x48,0x8D,0xA8,0x58,0xFF,0xFF}},
            {0x834C0,{0x4C,0x8D,0x05,0x59,0xFA,0x01,0x00,0xE9,0xB4,0xFE,0xFF,0xFF}},
            {0x42CA50,{0x48,0x83,0xEC,0x28,0x8B,0x05,0x8E,0xDA,0x60,0x05,0xA8,0x01}},
            {0x193F90,{0x48,0x89,0x5C,0x24,0x20,0x55,0x57,0x41,0x56,0x48,0x83,0xEC}},
            {0x194170,{0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30}},
            {0x99A90,{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0xB2,0x8C,0xE8}},
            {0xD262B0,{0x48,0x81,0xC1,0xB0,0x02,0x00,0x00,0x45,0x33,0xC0,0xE9,0x51}},
            {0xD47910,{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x48}}
        };
        template<class F> F function(std::uintptr_t rva) { return reinterpret_cast<F>(REL::Module::get().base() + rva); }
        bool matches(const player_inventory::Row& expected, const RE::BGSInventoryItem::Stack& stack, unsigned index)
        {
            if (expected.stackId != index || expected.count != stack.GetCount() || expected.stackIdentity != reinterpret_cast<std::uintptr_t>(&stack)) return false;
            const auto* instance=stack.extra?stack.extra->GetByType<RE::ExtraInstanceData>():nullptr;
            if(expected.instanceIdentity!=reinterpret_cast<std::uintptr_t>(instance?instance->data.get():nullptr)) return false;
            const auto uid = identity(stack.extra.get());
            return uid.base == expected.uniqueBase && uid.index == expected.uniqueIndex;
        }
        bool revalidate(RE::TESObjectREFR& container, const player_inventory::Row& row)
        {
            if (!container.inventoryList) return false;
            const RE::BSAutoReadLock lock(container.inventoryList->rwLock);
            for (const auto& item : container.inventoryList->data) if (item.object && item.object->formID == row.formId) {
                unsigned index{};
                for (auto* stack = item.stackData.get(); stack; stack = stack->nextStack.get(), ++index)
                    if (matches(row, *stack, index)) return true;
            }
            return false;
        }
    }
    bool ready()
    {
        static const bool valid = [] {
            for (const auto& guard : guards) if (std::memcmp(reinterpret_cast<const void*>(REL::Module::get().base() + guard.rva), guard.bytes.data(), guard.bytes.size())) {
                logger::error("Backpack storage native guard failed at {:X}", guard.rva); return false;
            }
            return true;
        }();
        return valid;
    }
    Identity identity(RE::ExtraDataList* extra)
    {
        Identity value;
        if (extra && ready()) function<void(*)(RE::ExtraDataList*, std::uint16_t*, std::uint32_t*)>(0x98830)(extra, &value.index, &value.base);
        return value;
    }
    Identity ensureIdentity(RE::TESObjectREFR& reference)
    {
        auto value = identity(reference.extraList.get());
        if (value || !ready() || !reference.extraList) return value;
        function<void(*)(RE::ExtraDataList*, std::uint16_t, std::uint32_t)>(0x98880)(reference.extraList.get(), 1, reference.formID);
        reference.AddChange(0x400);
        return identity(reference.extraList.get());
    }
    Identity pinWeapon(RE::TESObjectREFR& container,const player_inventory::Row& row)
    {
        if(!ready() || !container.inventoryList || row.equipped || !storage::isWeapon(row.kind) || !revalidate(container,row)) return {};
        if(row.count==1 && row.uniqueBase && row.uniqueIndex) return {row.uniqueBase,row.uniqueIndex};
        // The engine uses this free-index allocator before assigning a native
        // stack UID: AddObject 3E4144..415B and HandlePolicy 13672AA..7310.
        // It takes its own read lock, so call before acquiring our write lock.
        const auto index=function<std::uint16_t(*)(RE::TESObjectREFR*)>(0x3E3A10)(&container);
        if(!index) return {};
        struct Compare final:RE::BGSInventoryItem::StackDataCompareFunctor {
            const player_inventory::Row& expected; unsigned position{};
            explicit Compare(const player_inventory::Row& value):expected(value) {}
            bool CompareData(const RE::BGSInventoryItem::Stack& stack) override
            { return matches(expected,stack,position++) && !stack.IsEquipped(); }
        } compare(row);
        struct Writer final:RE::BGSInventoryItem::StackDataWriteFunctor {
            Identity desired,result;
            explicit Writer(Identity value):desired(value) { shouldSplitStacks=true; transferEquippedToSplitStack=false; }
            void WriteDataImpl(RE::TESBoundObject&,RE::BGSInventoryItem::Stack& stack) noexcept override try {
                if(stack.GetCount()!=1 || stack.IsEquipped()) return;
                if(!stack.extra) stack.extra=RE::BSTSmartPointer<RE::ExtraDataList>(new RE::ExtraDataList());
                function<void(*)(RE::ExtraDataList*,std::uint16_t,std::uint32_t)>(0x98880)(stack.extra.get(),desired.index,desired.base);
                result=identity(stack.extra.get());
            } catch(...) { result={}; logger::error("Native weapon identity writer failed; existing inventory unit preserved"); }
        } writer({container.formID,index});
        auto* object=RE::TESForm::GetFormByID<RE::TESBoundObject>(row.formId);
        if(!object) return {};
        // 1B4850 -> 1B0510 -> 1B0690 splits ONE unit before calling the writer,
        // then restacks compatible remainders. 1367310 is an independent UID
        // writer caller. Do not tag ammo/aid/etc.: 0C3210 compares UID values.
        using CompareObject=bool(*)(RE::TESBoundObject*,RE::TESBoundObject*);
        function<void(*)(RE::BGSInventoryList*,RE::TESBoundObject*,RE::BGSInventoryItem::StackDataCompareFunctor*,
            RE::BGSInventoryItem::StackDataWriteFunctor*,CompareObject,bool)>(0x1B4850)(container.inventoryList,object,&compare,&writer,
                +[](RE::TESBoundObject* a,RE::TESBoundObject* b){return a==b;},false);
        if(writer.result) container.AddChange(0x20);
        return writer.result;
    }
    std::uint32_t worldCount(RE::TESObjectREFR& reference)
    {
        if (!ready() || !reference.extraList) return 0;
        const auto count = function<int(*)(RE::ExtraDataList*)>(0x8BF60)(reference.extraList.get());
        return count > 0 ? unsigned(count) : 0;
    }
    RE::BSTEventSource<IdentityChange>* identityEvents()
    {
        return ready() ? function<RE::BSTEventSource<IdentityChange>*(*)()>(0x42CA50)() : nullptr;
    }
    RE::ObjectRefHandle createContainer(std::uint32_t baseForm)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::GetFormByID<RE::TESBoundObject>(baseForm);
        if (!ready() || !data || !player || !player->GetParentCell() || !base || base->GetFormType() != RE::ENUM_FORM_ID::kCONT) return {};
        RE::NEW_REFR_DATA creation;
        creation.object = base; creation.location = player->GetPosition();
        auto* cell = player->GetParentCell();
        creation.interior = cell->IsInterior() ? cell : nullptr;
        creation.world = cell->IsExterior() ? cell->worldSpace : nullptr;
        creation.forcePersist = true; creation.initiallyDisabled = true; creation.clearStillLoadingFlag = true;
        const auto handle = data->CreateReferenceAtLocation(creation);
        if (const auto ref = handle.get()) {
            ref->CreateInventoryList(base->As<RE::TESObjectCONT>());
            logger::info("Backpack storage container created {:08X}", ref->formID);
        }
        return handle;
    }
    RE::ObjectRefHandle createBackpack(std::uint32_t baseForm)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* base = RE::TESForm::GetFormByID<RE::TESBoundObject>(baseForm);
        if (!ready() || !player || !data || !base || base->GetFormType() != RE::ENUM_FORM_ID::kARMO || !player->GetParentCell()) return {};
        RE::NEW_REFR_DATA creation;
        creation.object = base; creation.location = player->GetPosition();
        const auto* cell = player->GetParentCell();
        creation.interior = cell->IsInterior() ? player->GetParentCell() : nullptr;
        creation.world = cell->IsExterior() ? cell->worldSpace : nullptr;
        creation.forcePersist = true; creation.initiallyDisabled = true;
        creation.clearStillLoadingFlag = true; creation.initializeScripts = false;
        return data->CreateReferenceAtLocation(creation);
    }
    void backpackSound(std::uint32_t baseForm, bool pickedUp)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::GetFormByID<RE::TESBoundObject>(baseForm);
        constexpr std::array<unsigned char, 9> expected{0x48,0x85,0xD2,0x0F,0x84,0x89,0,0,0};
        const auto address = REL::Module::get().base() + 0xF20720;
        static const bool verified = [address] {
            if (!std::memcmp(reinterpret_cast<void*>(address), expected.data(), expected.size())) return true;
            logger::error("Backpack sound native guard failed at F20720; sound unavailable"); return false;
        }();
        if (!player || !base || !verified) return;
        // F20720 plays the sound only. DC13D0 forwards R8 (pickup) to the
        // object's pickup/putdown resolver; neither path queues item messages.
        reinterpret_cast<void(*)(RE::PlayerCharacter*,RE::TESBoundObject*,bool,bool)>(address)(player,base,pickedUp,false);
    }
    bool deposit(RE::TESObjectREFR& container, RE::TESObjectREFR& reference, bool identify)
    {
        if (!ready() || reference.GetFormType() != RE::ENUM_FORM_ID::kREFR || reference.IsDeleted() || reference.IsDisabled() || &container == &reference || !container.GetObjectReference() ||
            (container.GetObjectReference()->GetFormType() != RE::ENUM_FORM_ID::kCONT && &container != RE::PlayerCharacter::GetSingleton())) return false;
        auto* object = reference.GetObjectReference();
        const auto count = worldCount(reference);
        if (!object || !count || !player_inventory::describe(reference).formId || (identify && !ensureIdentity(reference))) return false;
        if (auto* handles = reference.extraList->GetByType<RE::ExtraReferenceHandles>(); handles && handles->containerRef.get()) return false;
        const auto before = container.GetInventoryObjectCount(object);
        // Both Papyrus AddItem(ref) and native console AddItem(ref) use this
        // ordered pair. The first retires the world representation; the second
        // transfers its extra data and notifies native container/identity events.
        function<void(*)(RE::TESObjectREFR*)>(0x3F6FE0)(&reference);
        function<void(*)(RE::TESObjectREFR*, RE::TESObjectREFR*, std::uint32_t)>(0x3E15E0)(&container, &reference, count);
        const auto after = container.GetInventoryObjectCount(object);
        if (after >= before && after - before == count) return true;
        logger::error("Backpack native deposit unconfirmed: reference={:08X}, container={:08X}, before={}, after={}, quantity={}",
            reference.formID, container.formID, before, after, count);
        // Never manufacture a second copy after an uncertain native transfer.
        return false;
    }
    RE::ObjectRefHandle withdraw(RE::TESObjectREFR& container, const player_inventory::Row& row, std::uint32_t quantity, const RE::NiPoint3& position)
    {
        if (!quantity || quantity > row.count || quantity > 32767 || !revalidate(container, row)) return {};
        auto* object = RE::TESForm::GetFormByID<RE::TESBoundObject>(row.formId);
        if (!object) return {};
        if(&container==RE::PlayerCharacter::GetSingleton()) {
            if(object->Is(RE::ENUM_FORM_ID::kKEYM)) return {};
            const RE::BSAutoReadLock lock(container.inventoryList->rwLock);
            for(const auto& item:container.inventoryList->data) if(item.object==object &&
                function<bool(*)(const RE::BGSInventoryItem*,std::int32_t)>(0x1AE3F0)(&item,static_cast<std::int32_t>(row.stackId)))
                return {}; // Same exact-stack quest check as native Pip-Boy/container removal.
        }
        RE::TESObjectREFR::RemoveItemData removal(object, int(quantity));
        removal.reason = RE::ITEM_REMOVE_REASON::KDropping; removal.dropLoc = &position; removal.stackData.push_back(row.stackId);
        return container.RemoveItem(removal);
    }
    bool transfer(RE::TESObjectREFR& source, RE::TESObjectREFR& destination, const player_inventory::Row& row, std::uint32_t quantity)
    {
        if (&source == &destination || !quantity || quantity > row.count || quantity > INT_MAX || !revalidate(source, row)) return false;
        auto* object = RE::TESForm::GetFormByID<RE::TESBoundObject>(row.formId);
        if (!object) return false;
        const auto before = destination.GetInventoryObjectCount(object);
        RE::TESObjectREFR::RemoveItemData removal(object, int(quantity));
        removal.reason = RE::ITEM_REMOVE_REASON::kStoreContainer; removal.a_otherContainer = &destination; removal.stackData.push_back(row.stackId);
        (void)source.RemoveItem(removal);
        const auto after = destination.GetInventoryObjectCount(object);
        return after >= before && after - before == quantity;
    }
    ExtraLease extra(RE::TESObjectREFR& container, const player_inventory::Row& row)
    {
        if (!container.inventoryList) return {};
        const RE::BSAutoReadLock lock(container.inventoryList->rwLock);
        for (auto& item : container.inventoryList->data) if (item.object && item.object->formID == row.formId) {
            unsigned index{};
            for (auto* stack = item.stackData.get(); stack; stack = stack->nextStack.get(), ++index)
                if (matches(row, *stack, index)) return ExtraLease(stack->extra.get());
        }
        return {};
    }
    void releaseExtra(RE::ExtraDataList* value) noexcept
    {
        if(value && value->DecRef()==0) {
            // Native smart-pointer assignment 12C280 destroys via 813A0 before
            // releasing the allocation. Preserve that exact final-owner path.
            function<void(*)(RE::ExtraDataList*)>(0x813A0)(value);
            RE::free(value);
        }
    }
    bool compatible(RE::ExtraDataList* first, RE::ExtraDataList* second)
    {
        // CompareList returns "different". Native stack comparator 1AD960
        // accepts only AL == 0 after calling 834C0.
        if(!ready()) return false;
        RE::ExtraDataList empty;
        return !function<bool(*)(RE::ExtraDataList*, RE::ExtraDataList*)>(0x834C0)(first?first:&empty,second?second:&empty);
    }
    bool current(RE::TESObjectREFR& owner,const player_inventory::Row& row)
    { return ready() && revalidate(owner,row); }
    bool park(RE::TESObjectREFR& reference)
    {
        if (!ready() || reference.IsDeleted()) return false;
        function<void(*)(RE::TESObjectREFR*)>(0x3F63C0)(&reference);
        return reference.IsDisabled();
    }
    void loadSavedReference(std::uint32_t reference)
    {
        // FO4VR raw callers 140368F50 and 14036B1C0 both lookup, call D262B0
        // with (save manager, exact form ID), then lookup again before retaining.
        // D47910 restores the native change record, including instance extras.
        // This migration needs created FF references, not arbitrary static forms.
        if(!ready() || (reference>>24)!=0xFF || RE::TESForm::GetFormByID(reference)) return;
        auto* manager=*reinterpret_cast<void**>(REL::Module::get().base()+0x5AD5A80);
        if(!manager) { logger::error("Stored-reference load unavailable: no save manager for {:08X}",reference); return; }
        function<void(*)(void*,std::uint32_t)>(0xD262B0)(manager,reference);
    }
    bool retainStoredReference(RE::TESObjectREFR& reference, RE::TESObjectREFR& owner)
    {
        if(!ready() || reference.IsDeleted() || owner.IsDeleted() || !reference.extraList || &reference==&owner) return false;
        // ForcePersistent console caller 1405176E0 and reference-binding callers
        // 140368F50/14036B1C0 agree: RDX=reference, R8=persistence owner.
        // 193F90 promotes through 3DD0A0, adds the owner to extra 8C, and marks
        // save change 02000000. A NiPointer alone does none of those things.
        const auto contains=function<bool(*)(RE::ExtraDataList*,RE::TESForm*)>(0x99A90);
        if(!contains(reference.extraList.get(),&owner)) {
            auto* manager=*reinterpret_cast<void**>(REL::Module::get().base()+0x5932310);
            if(!manager || !function<bool(*)(void*,RE::TESObjectREFR*,RE::TESForm*)>(0x193F90)(manager,&reference,&owner)) return false;
        }
        return (reference.formFlags & RE::TESForm::kFormFlag_Persistent) && contains(reference.extraList.get(),&owner);
    }
    bool releaseStoredReference(RE::TESObjectREFR& reference, RE::TESObjectREFR& owner)
    {
        if(!ready() || !reference.extraList) return false;
        const auto contains=function<bool(*)(RE::ExtraDataList*,RE::TESForm*)>(0x99A90);
        if(!contains(reference.extraList.get(),&owner)) return true;
        auto* manager=*reinterpret_cast<void**>(REL::Module::get().base()+0x5932310);
        if(!manager) return false;
        // Remove only this bag's owner; other quests/mods retain their claims.
        // 194170's boolean describes demotion, not whether our owner was removed.
        (void)function<bool(*)(void*,RE::TESObjectREFR*,RE::TESForm*,bool)>(0x194170)(manager,&reference,&owner,false);
        return !contains(reference.extraList.get(),&owner);
    }
    bool showAt(RE::TESObjectREFR& reference, const RE::NiPoint3& position)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ready() || !player || !player->GetParentCell() || reference.IsDeleted()) return false;
        const auto delta = position - player->GetPosition();
        function<void(*)(RE::TESObjectREFR*, RE::TESObjectREFR*, float, float, float)>(0x4E6E00)(&reference, player, delta.x, delta.y, delta.z);
        function<void(*)(RE::TESObjectREFR*, bool)>(0x3F5DA0)(&reference, false);
        if (!reference.IsDisabled() && reference.GetObjectReference() &&
            reference.GetObjectReference()->Is(RE::ENUM_FORM_ID::kWEAP))
            weapon_material_fix::referenceChanged(reference.formID);
        return !reference.IsDisabled();
    }
}
