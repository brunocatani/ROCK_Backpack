#include "PCH.h"

#include "PlayerInventory.h"
#include "ui/ItemIconPolicy.h"
#include "NativeInventory.h"

#include "RE/Bethesda/Actor.h"
#include "RE/Bethesda/ActorValueInfo.h"
#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BSExtraData.h"
#include "RE/Bethesda/BSLock.h"
#include "RE/Bethesda/MagicItems.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESObjectREFRs.h"
#include "RE/Bethesda/TESRace.h"
#include "RE/Bethesda/IMenu.h"
#include "RE/Bethesda/UIMessageQueue.h"

#include <array>
#include <cctype>
#include <cmath>
#include <tuple>

namespace rock_backpack::player_inventory
{
    namespace
    {
        using inventory_policy::Category;

        struct ClassifiedItem
        {
            Category category{ Category::Weapons };
            RE::TESBoundObject* object{ nullptr };
            RE::TESObjectWEAP* weapon{ nullptr };
            RE::TESObjectARMO* armor{ nullptr };
            const char* baseName{ nullptr };
        };

        struct RuntimeWeaponTarget
        {
            RE::TESObjectWEAP* weapon{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData{};
            RE::BGSEquipSlot* equipSlot{ nullptr };
            std::uint32_t stackId{ 0 };
            bool equipped{ false };
        };

        struct RuntimeArmorTarget
        {
            RE::TESObjectARMO* armor{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData{};
            RE::BGSEquipSlot* equipSlot{ nullptr };
            std::uint32_t stackId{ 0 };
            bool equipped{ false };
        };

        // Category-agnostic counterpart to the weapon and armor targets above.
        // Consume, drop and force-grab all act on a stack rather than on an
        // equip slot, so they share this one revalidated lookup.
        struct RuntimeItemTarget
        {
            RE::TESBoundObject* object{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData{};
            std::uint32_t stackId{ 0 };
            std::uint32_t count{ 0 };
        };

        class RevisionBuilder
        {
        public:
            void addContext(
                const std::uint32_t raceFormId,
                const std::uint32_t pipboyMask) noexcept
            {
                add(raceFormId);
                add(pipboyMask);
            }

            void addRow(
                const Category category,
                const std::uint32_t formId,
                const std::uint32_t stackId,
                const std::uint32_t count,
                const std::uint32_t armorSlotMask,
                const std::uint32_t armorZoneFlags,
                const bool equipped,
                const std::uintptr_t stackIdentity,
                const std::uintptr_t instanceIdentity,
                const char* name) noexcept
            {
                add(static_cast<std::uint32_t>(category));
                add(formId);
                add(stackId);
                add(count);
                add(armorSlotMask);
                add(armorZoneFlags);
                add(static_cast<std::uint32_t>(equipped));
                add64(stackIdentity);
                add64(instanceIdentity);
                addString(name);
                ++_rowCount;
            }

            [[nodiscard]] Revision finish(const bool available) const noexcept
            {
                return {
                    .available = available,
                    .rowCount = _rowCount,
                    .hashA = _hashA,
                    .hashB = _hashB,
                };
            }

        private:
            void add(const std::uint32_t value) noexcept
            {
                addByte(static_cast<unsigned char>(value & 0xFFu));
                addByte(static_cast<unsigned char>((value >> 8) & 0xFFu));
                addByte(static_cast<unsigned char>((value >> 16) & 0xFFu));
                addByte(static_cast<unsigned char>((value >> 24) & 0xFFu));
            }

            void add64(const std::uint64_t value) noexcept
            {
                add(static_cast<std::uint32_t>(value & 0xFFFF'FFFFull));
                add(static_cast<std::uint32_t>(value >> 32));
            }

            void addString(const char* value) noexcept
            {
                if (value) {
                    for (const auto* cursor =
                             reinterpret_cast<const unsigned char*>(value);
                         *cursor;
                         ++cursor) {
                        addByte(*cursor);
                    }
                }
                addByte(0xFFu);
            }

            void addByte(const unsigned char value) noexcept
            {
                _hashA ^= value;
                _hashA *= 1099511628211ull;
                _hashB ^= static_cast<std::uint64_t>(value) +
                          0x9E3779B97F4A7C15ull +
                          (_hashB << 6) + (_hashB >> 2);
            }

            std::uint32_t _rowCount{ 0 };
            std::uint64_t _hashA{ 14695981039346656037ull };
            std::uint64_t _hashB{ 0x6A09E667F3BCC909ull };
        };

        /*
         * Holotape / note playback.
         *
         * Traced out of Fallout4VR.exe rather than guessed. The Pip-Boy's own
         * HolotapeSelectionCallback (ctor 0x140be84e0, vtable 0x142d5d370) keeps
         * its invoke operator in vtable slot 1 at 0x140be8d60. That operator
         * bounds checks the chosen index against a count at +0x20, loads the
         * note's FormID out of the array at +0x10, fetches a lazily constructed
         * singleton (getter 0x1420d4f20, instance 0x1465d8d20) and calls a
         * dispatcher at 0x1420d93f0 with mode 6 - kHolotape in IMenu.h's menu
         * enum. The dispatcher wraps the FormID in a 0x30-byte parameter object
         * holding it at +0x28, hands that to the UI manager at 0x140b8bb50, then
         * releases it through its deleting destructor.
         *
         * Calling that dispatcher is therefore exactly what selecting the tape in
         * the Pip-Boy does, which is why this reuses the engine's entry point
         * instead of driving the four NOTE_TYPE behaviours (voice, scene, program,
         * terminal) separately - the engine already dispatches on type behind it.
         *
         * Address-library IDs resolved from version-1-2-72-0.csv, the 1.2.72.0 VR
         * database, so no raw address is hardcoded:
         *     905248 -> 0x020d4f20  selection singleton getter
         *     141019 -> 0x020d93f0  dispatch(singleton, mode, formID)
         *
         * Failure behaviour: the dispatcher tests its own manager global and
         * returns immediately when it is null, so calling it outside a live UI
         * context is a no-op rather than a crash. The singleton is checked anyway.
         */
        constexpr std::uint32_t kHolotapeMenuMode = 6;

        [[nodiscard]] bool playNote(RE::BGSNote& note) noexcept
        {
            using selection_singleton_t = void*();
            using dispatch_menu_t = void(void*, std::uint32_t, std::uint32_t);

            static REL::Relocation<selection_singleton_t> selectionSingleton{
                REL::ID(905248)
            };
            static REL::Relocation<dispatch_menu_t> dispatchMenu{
                REL::ID(141019)
            };
            /*
             * The pointer the dispatcher guards on at 0x145932558 turned out to be
             * the UI message queue: CommonLibF4VR resolves
             * UIMessageQueue::GetSingleton through RelocationID(82123, ...), the
             * same 82123 this address maps to. So it is read through the typed
             * accessor rather than a raw slot.
             *
             * That identification also explains what 0x140b8bb50 does. It takes a
             * lock at queue+0x218, builds the message payload, then FILES the
             * parameter object into a per-mode slot in the queue's array, releasing
             * whatever was there. It stages data for a menu mode - it never shows
             * the menu. Which is exactly what the first attempt observed: the trace
             * reported manager=live and the dispatch returned normally, yet nothing
             * appeared, because the parameters were filed correctly and nobody
             * asked the UI to open.
             *
             * So playback is two steps: stage the tape against mode 6 (kHolotape),
             * then post a show message for HolotapeMenu.
             */
            auto* instance = selectionSingleton();
            auto* queue = RE::UIMessageQueue::GetSingleton();

            /*
             * User-initiated, so one line per button press cannot flood. The fields
             * discriminate: noteType localises which NOTE_TYPE behaviour is in play,
             * and queue/singleton separate a bad address from a refused request.
             */
            logger::info(
                "ROCK Backpack HOLOTAPE-DISPATCH form={:08X} noteType={} singleton={} queue={}",
                note.GetFormID(),
                static_cast<std::int32_t>(note.type),
                instance ? "ok" : "null",
                queue ? "live" : "NULL");

            if (!instance || !queue) {
                logger::warn(
                    "ROCK Backpack cannot play '{}': holotape dispatch is unavailable (singleton={}, queue={}).",
                    note.GetFullName() ? note.GetFullName() : "",
                    instance ? "ok" : "null",
                    queue ? "live" : "null");
                return false;
            }

            // Stage the selected tape against the holotape mode...
            dispatchMenu(instance, kHolotapeMenuMode, note.GetFormID());
            // ...then ask the UI to actually open it. Without this the staged
            // parameters just sit in the queue's slot, which is the bug the first
            // attempt hit.
            queue->AddMessage(
                RE::HolotapeMenu::MENU_NAME,
                RE::UI_MESSAGE_TYPE::kShow);
            return true;
        }

        // Aid is consumed; a note is played. Nothing else has a meaningful use.
        [[nodiscard]] bool isUsableCategory(
            const ClassifiedItem& classified) noexcept
        {
            if (classified.category == Category::Aid) {
                return true;
            }
            return classified.category == Category::Misc &&
                   classified.object != nullptr &&
                   classified.object->As<RE::BGSNote>() != nullptr;
        }

        [[nodiscard]] std::optional<ClassifiedItem> classify(
            RE::TESBoundObject* object) noexcept
        {
            if (!object) {
                return std::nullopt;
            }
            if (auto* weapon = object->As<RE::TESObjectWEAP>()) {
                return ClassifiedItem{
                    .category = Category::Weapons,
                    .object = object,
                    .weapon = weapon,
                    .baseName = weapon->GetFullName(),
                };
            }
            if (auto* armor = object->As<RE::TESObjectARMO>()) {
                return ClassifiedItem{
                    .category = Category::Armor,
                    .object = object,
                    .armor = armor,
                    .baseName = armor->GetFullName(),
                };
            }
            if (auto* aid = object->As<RE::AlchemyItem>()) {
                return ClassifiedItem{
                    .category = Category::Aid,
                    .object = object,
                    .baseName = aid->GetFullName(),
                };
            }
            if (auto* junk = object->As<RE::TESObjectMISC>();
                junk && !junk->IsLooseMod() && junk->componentData &&
                !junk->componentData->empty()) {
                return ClassifiedItem{
                    .category = Category::Junk,
                    .object = object,
                    .baseName = junk->GetFullName(),
                };
            }
            if (auto* ammo = object->As<RE::TESAmmo>()) {
                return ClassifiedItem{
                    .category = Category::Ammo,
                    .object = object,
                    .baseName = ammo->GetFullName(),
                };
            }
            /*
             * Holotapes and notes are NOTE records, not BOOK. Fallout 4 kept BOOK
             * as a near-unused vestige and moved every holotape, audio log and
             * terminal program to BGSNote, so classifying only BOOK left every
             * holotape the player carries invisible to this panel.
             */
            if (auto* note = object->As<RE::BGSNote>()) {
                return ClassifiedItem{
                    .category = Category::Misc,
                    .object = object,
                    .baseName = note->GetFullName(),
                };
            }
            if (auto* book = object->As<RE::TESObjectBOOK>()) {
                return ClassifiedItem{
                    .category = Category::Misc,
                    .object = object,
                    .baseName = book->GetFullName(),
                };
            }
            /*
             * Everything else carryable that is not scrap. TESKey derives from
             * TESObjectMISC and carries no component data, so the junk test above
             * declines it and it lands here, which is what makes keys visible at
             * all. Loose weapon mods are ordinary stored inventory items too.
             */
            if (auto* misc = object->As<RE::TESObjectMISC>(); misc) {
                return ClassifiedItem{
                    .category = Category::Misc,
                    .object = object,
                    .baseName = misc->GetFullName(),
                };
            }
            return std::nullopt;
        }

        storage::Kind storageKind(const ClassifiedItem& item, const RE::TBO_InstanceData* instance)
        {
            if (item.weapon) {
                const auto* data = instance ? static_cast<const RE::TESObjectWEAP::InstanceData*>(instance) : &item.weapon->weaponData;
                const auto shape = static_cast<unsigned>(*data->type);
                const auto has = [&](std::uint32_t id) {
                    const auto* keyword = RE::TESForm::GetFormByID<RE::BGSKeyword>(id);
                    return keyword && item.weapon->HasKeyword(keyword,instance);
                };
                // The same effective WeaponType* keywords published by regular
                // ROCK. A scope or a display name alone does not make a sniper.
                bool heavy = has(0x0004A0A3);
                for (const auto id : {0x0022575Du,0x0022575Cu,0x0022575Bu,0x0022575Eu,
                        0x00225760u,0x0022575Fu,0x00225763u,0x00225766u}) heavy |= has(id);
                return storage::weaponKind(shape<=6,shape==2,has(0x0004A0A0),has(0x001E325D),heavy,shape==10 || shape==11);
            }
            switch (item.category) {
            case Category::Armor: return storage::Kind::Armor;
            case Category::Aid: return storage::Kind::Aid;
            case Category::Ammo: return storage::Kind::Ammo;
            case Category::Junk: return storage::Kind::Junk;
            default: return storage::Kind::Misc;
            }
        }

        [[nodiscard]] palm::api::Icon resolveIcon(
            const ClassifiedItem& classified,
            const RE::TBO_InstanceData* instanceData,
            const std::uint32_t /*armorZoneFlags*/) noexcept
        {
            using ui::Icon;
            auto* object = classified.object;
            if (!object) return Icon::OpenHand;
            const auto* file = object->GetFile(0);
            const auto known = ui::knownItemIcon(file && _stricmp(file->filename, "Fallout4.esm") == 0,
                object->GetFormID() & 0xFFFFFFu);
            if (classified.weapon) {
                return ui::weaponIcon(known, static_cast<std::uint8_t>(*classified.weapon->weaponData.type),
                    [&](std::uint32_t id) {
                        const auto* keyword = RE::TESForm::GetFormByID<RE::BGSKeyword>(id);
                        return keyword && classified.weapon->HasKeyword(keyword, instanceData);
                    });
            }
            if (auto* armor = object->As<RE::TESObjectARMO>()) {
                const auto* power = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004D8A1);
                return ui::armorIcon(armor->GetSlotMask(), power && armor->HasKeyword(power, instanceData));
            }
            if (known != Icon::Automatic) return known;
            if (auto* aid = object->As<RE::AlchemyItem>()) return aid->IsFood() ? Icon::Food : Icon::Chems;
            // PALM has no ammo/junk/holotape-specific artwork; use its category
            // symbols while keeping the exact item name visible.
            if (classified.category == Category::Ammo) return Icon::Grenades;
            if (classified.category == Category::Junk) return Icon::Config;
            return Icon::OpenHand;
        }

        [[nodiscard]] RE::TBO_InstanceData* stackInstanceData(
            const RE::BGSInventoryItem::Stack& stack) noexcept
        {
            if (!stack.extra) {
                return nullptr;
            }
            const auto* instance =
                stack.extra->GetByType<RE::ExtraInstanceData>();
            return instance ? instance->data.get() : nullptr;
        }

        [[nodiscard]] std::uint32_t countStacks(
            const RE::BGSInventoryItem& item) noexcept
        {
            std::uint32_t count = 0;
            for (auto* stack = item.stackData.get(); stack;
                 stack = stack->nextStack.get()) {
                if (stack->GetCount() != 0) {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] bool isEquippedWeaponStack(
            const RE::PlayerCharacter& player,
            const RE::TESObjectWEAP& weapon,
            const RE::BGSInventoryItem::Stack& stack,
            const std::uint32_t stackCount) noexcept
        {
            if (stack.IsEquipped()) {
                return true;
            }

            const auto* middleHigh =
                player.currentProcess ?
                    player.currentProcess->middleHigh :
                    nullptr;
            if (!middleHigh) {
                return false;
            }

            const auto* stackInstance = stackInstanceData(stack);
            for (const auto& equippedItem :
                 middleHigh->equippedItems) {
                if (equippedItem.item.object != &weapon) {
                    continue;
                }
                const auto* equippedInstance =
                    equippedItem.item.instanceData.get();
                if (stackInstance && equippedInstance) {
                    if (stackInstance == equippedInstance) {
                        return true;
                    }
                    continue;
                }
                if (stackCount == 1) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::uint32_t pipboySlotMask(
            const RE::PlayerCharacter& player) noexcept
        {
            if (!player.race) {
                return 0;
            }
            return inventory_policy::editorIndexMask(
                player.race->data.pipboyObject.underlying());
        }

        [[nodiscard]] bool armorIsVisibleAndMutable(
            const RE::PlayerCharacter& player,
            const RE::TESObjectARMO& armor,
            const RE::TBO_InstanceData* instanceData,
            const std::uint32_t slotMask) noexcept
        {
            return armor.GetPlayable(instanceData) &&
                   (slotMask & pipboySlotMask(player)) == 0;
        }

        [[nodiscard]] const char* resolvedDisplayName(
            RE::BGSInventoryItem& item,
            const ClassifiedItem& classified,
            const std::uint32_t stackId,
            char (&fallback)[48]) noexcept
        {
            if (const char* name = item.GetDisplayFullName(stackId);
                name && *name) {
                return name;
            }
            if (classified.baseName && *classified.baseName) {
                return classified.baseName;
            }

            const int written = std::snprintf(
                fallback,
                sizeof(fallback),
                "%.*s %08X",
                static_cast<int>(
                    inventory_policy::categoryKey(classified.category).size()),
                inventory_policy::categoryKey(classified.category).data(),
                classified.object ? classified.object->GetFormID() : 0);
            return written > 0 &&
                           static_cast<std::size_t>(written) <
                               sizeof(fallback) ?
                       fallback :
                       "Unnamed item";
        }

        /*
         * Verified against Fallout4VR.exe at RVA 0x170ee0 (address-library id
         * 1321341): the resolver null-checks the instance-data argument, calls
         * TBO_InstanceData::GetWeight through vtable slot 0x0D, and only when
         * that returns the -1.0f sentinel does it dispatch on the form type to
         * read the per-type weight member. One call therefore covers weapons,
         * armor, aid, junk and ammo including mod-adjusted instance weights,
         * which is why no per-type branching or InstanceData layout assumption
         * is made here.
         */
        [[nodiscard]] float resolvedWeight(
            const RE::TESBoundObject& object,
            const RE::TBO_InstanceData* instanceData) noexcept
        {
            const float weight =
                RE::TESWeightForm::GetFormWeight(&object, instanceData);
            return std::isfinite(weight) && weight > 0.0f ? weight : 0.0f;
        }

        [[nodiscard]] std::string armorSlotLabel(
            const RE::PlayerCharacter& player,
            const std::uint32_t slotMask)
        {
            if (slotMask == 0) {
                return "UTILITY / OTHER";
            }

            std::string result;
            std::uint32_t included = 0;
            std::uint32_t remaining = 0;
            for (std::uint32_t index = 0; index < 32; ++index) {
                if ((slotMask & (1u << index)) == 0) {
                    continue;
                }
                if (included >= 2) {
                    ++remaining;
                    continue;
                }

                const char* name = nullptr;
                if (player.race) {
                    name = player.race->bipedObjectNames[index].c_str();
                }
                char fallback[24]{};
                if (!name || !*name) {
                    const int written = std::snprintf(
                        fallback,
                        sizeof(fallback),
                        "SLOT %u",
                        index + 30);
                    name = written > 0 &&
                                   static_cast<std::size_t>(written) <
                                       sizeof(fallback) ?
                               fallback :
                               "OTHER";
                }
                if (!result.empty()) {
                    result += " / ";
                }
                result += name;
                ++included;
            }
            if (remaining != 0) {
                result += " +";
                result += std::to_string(remaining);
            }
            return result.empty() ? "UTILITY / OTHER" : result;
        }

        template <class Visitor>
        void visitVisibleStacks(
            RE::PlayerCharacter& player,
            Visitor&& visitor,
            RE::TESObjectREFR* source = nullptr)
        {
            if (!source) source = &player;
            const std::uint32_t protectedPipboyMask =
                pipboySlotMask(player);
            const RE::BSAutoReadLock inventoryLock{
                source->inventoryList->rwLock
            };
            for (auto& item : source->inventoryList->data) {
                auto classified = classify(item.object);
                if (!classified) {
                    continue;
                }

                const std::uint32_t stackCount =
                    classified->weapon ? countStacks(item) : 0;
                std::uint32_t stackId = 0;
                for (auto* stack = item.stackData.get(); stack;
                     stack = stack->nextStack.get(), ++stackId) {
                    const std::uint32_t count = stack->GetCount();
                    if (count == 0) {
                        continue;
                    }
                    auto* instanceData = stackInstanceData(*stack);
                    const std::uint32_t armorSlotMask =
                        classified->armor ?
                            classified->armor->GetSlotMask() :
                            0;
                    if (classified->armor &&
                        (!classified->armor->GetPlayable(instanceData) ||
                         (armorSlotMask & protectedPipboyMask) != 0)) {
                        continue;
                    }

                    char fallback[48]{};
                    const char* name = resolvedDisplayName(
                        item,
                        *classified,
                        stackId,
                        fallback);
                    visitor(
                        *classified,
                        *stack,
                        stackId,
                        count,
                        instanceData,
                        name,
                        armorSlotMask,
                        classified->armor ?
                            inventory_policy::armorZoneFlags(
                                armorSlotMask) :
                            0,
                        source == &player && (classified->weapon ?
                            isEquippedWeaponStack(
                                player,
                                *classified->weapon,
                                *stack,
                                stackCount) :
                            classified->armor && stack->IsEquipped()));
                }
            }
        }

        [[nodiscard]] std::string makeRowId(
            const Category category,
            const std::uint32_t formId,
            const std::uint32_t stackId,
            const std::size_t rowIndex)
        {
            char value[56]{};
            const int written = std::snprintf(
                value,
                sizeof(value),
                "%u:%08X:%u:%zu",
                static_cast<std::uint32_t>(category),
                formId,
                stackId,
                rowIndex);
            return written > 0 &&
                           static_cast<std::size_t>(written) <
                               sizeof(value) ?
                       std::string{
                           value,
                           static_cast<std::size_t>(written)
                       } :
                       std::string{};
        }

        [[nodiscard]] unsigned char asciiFold(const char value) noexcept
        {
            const auto byte = static_cast<unsigned char>(value);
            return byte >= static_cast<unsigned char>('A') &&
                           byte <= static_cast<unsigned char>('Z') ?
                       static_cast<unsigned char>(byte + ('a' - 'A')) :
                       byte;
        }

        [[nodiscard]] bool rowLess(
            const Row& left,
            const Row& right) noexcept
        {
            if (left.category != right.category) {
                return left.category < right.category;
            }
            if (left.category == Category::Armor &&
                left.equipped != right.equipped) {
                return left.equipped > right.equipped;
            }
            const auto length =
                (std::min)(left.name.size(), right.name.size());
            for (std::size_t index = 0; index < length; ++index) {
                const auto leftByte = asciiFold(left.name[index]);
                const auto rightByte = asciiFold(right.name[index]);
                if (leftByte != rightByte) {
                    return leftByte < rightByte;
                }
            }
            if (left.name.size() != right.name.size()) {
                return left.name.size() < right.name.size();
            }
            return std::tie(
                       left.formId,
                       left.stackId,
                       left.stackIdentity) <
                   std::tie(
                       right.formId,
                       right.stackId,
                       right.stackIdentity);
        }

        [[nodiscard]] std::optional<RuntimeArmorTarget>
        findRuntimeArmorTarget(
            RE::PlayerCharacter& player,
            const Row& expected) noexcept
        {
            if (!player.inventoryList ||
                expected.category != Category::Armor) {
                return std::nullopt;
            }

            const RE::BSAutoReadLock inventoryLock{
                player.inventoryList->rwLock
            };
            for (auto& item : player.inventoryList->data) {
                auto* armor =
                    item.object ?
                        item.object->As<RE::TESObjectARMO>() :
                        nullptr;
                if (!armor || armor->GetFormID() != expected.formId) {
                    continue;
                }

                std::uint32_t stackId = 0;
                for (auto* stack = item.stackData.get(); stack;
                     stack = stack->nextStack.get(), ++stackId) {
                    if (stackId != expected.stackId ||
                        stack->GetCount() == 0) {
                        continue;
                    }
                    if (stack->GetCount() != expected.count ||
                        reinterpret_cast<std::uintptr_t>(stack) !=
                            expected.stackIdentity) {
                        continue;
                    }

                    RE::BSTSmartPointer<RE::TBO_InstanceData>
                        instanceData{};
                    if (stack->extra) {
                        if (const auto* instance =
                                stack->extra->GetByType<
                                    RE::ExtraInstanceData>()) {
                            instanceData = instance->data;
                        }
                    }
                    if (reinterpret_cast<std::uintptr_t>(
                            instanceData.get()) !=
                        expected.instanceIdentity) {
                        continue;
                    }

                    const std::uint32_t slotMask =
                        armor->GetSlotMask();
                    if (slotMask != expected.armorSlotMask ||
                        inventory_policy::armorZoneFlags(slotMask) !=
                            expected.armorZoneFlags ||
                        !armorIsVisibleAndMutable(
                            player,
                            *armor,
                            instanceData.get(),
                            slotMask)) {
                        continue;
                    }
                    const bool equipped = stack->IsEquipped();
                    if (equipped != expected.equipped) {
                        continue;
                    }

                    auto* equipSlot =
                        armor->GetEquipSlot(instanceData.get());
                    if (!equipSlot) {
                        equipSlot = armor->GetEquipSlot(nullptr);
                    }
                    return RuntimeArmorTarget{
                        .armor = armor,
                        .instanceData = std::move(instanceData),
                        .equipSlot = equipSlot,
                        .stackId = stackId,
                        .equipped = equipped,
                    };
                }
            }
            return std::nullopt;
        }

        /*
         * Applies the same staleness contract the armor and weapon lookups
         * use: the base form, stack index, stack count, live stack pointer and
         * live instance pointer must all still match the snapshot the panel
         * acted on. Anything else means the inventory moved under the user, and
         * the caller refuses the action rather than mutating a different stack
         * that happens to occupy the old index.
         */
        [[nodiscard]] std::optional<RuntimeItemTarget>
        findRuntimeItemTarget(
            RE::PlayerCharacter& player,
            const Row& expected) noexcept
        {
            if (!player.inventoryList) {
                return std::nullopt;
            }

            const RE::BSAutoReadLock inventoryLock{
                player.inventoryList->rwLock
            };
            for (auto& item : player.inventoryList->data) {
                if (!item.object ||
                    item.object->GetFormID() != expected.formId) {
                    continue;
                }
                const auto classified = classify(item.object);
                if (!classified ||
                    classified->category != expected.category) {
                    continue;
                }

                std::uint32_t stackId = 0;
                for (auto* stack = item.stackData.get(); stack;
                     stack = stack->nextStack.get(), ++stackId) {
                    if (stackId != expected.stackId ||
                        stack->GetCount() == 0) {
                        continue;
                    }
                    if (stack->GetCount() != expected.count ||
                        reinterpret_cast<std::uintptr_t>(stack) !=
                            expected.stackIdentity) {
                        continue;
                    }

                    RE::BSTSmartPointer<RE::TBO_InstanceData>
                        instanceData{};
                    if (stack->extra) {
                        if (const auto* instance =
                                stack->extra->GetByType<
                                    RE::ExtraInstanceData>()) {
                            instanceData = instance->data;
                        }
                    }
                    if (reinterpret_cast<std::uintptr_t>(
                            instanceData.get()) !=
                        expected.instanceIdentity) {
                        continue;
                    }

                    return RuntimeItemTarget{
                        .object = item.object,
                        .instanceData = std::move(instanceData),
                        .stackId = stackId,
                        .count = stack->GetCount(),
                    };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<RuntimeWeaponTarget>
        findRuntimeWeaponTarget(
            RE::PlayerCharacter& player,
            const Row& expected) noexcept
        {
            if (!player.inventoryList ||
                expected.category != Category::Weapons) {
                return std::nullopt;
            }

            const RE::BSAutoReadLock inventoryLock{
                player.inventoryList->rwLock
            };
            for (auto& item : player.inventoryList->data) {
                auto* weapon =
                    item.object ?
                        item.object->As<RE::TESObjectWEAP>() :
                        nullptr;
                if (!weapon ||
                    weapon->GetFormID() != expected.formId) {
                    continue;
                }

                const std::uint32_t stackCount =
                    countStacks(item);
                std::uint32_t stackId = 0;
                for (auto* stack = item.stackData.get(); stack;
                     stack = stack->nextStack.get(), ++stackId) {
                    if (stackId != expected.stackId ||
                        stack->GetCount() == 0) {
                        continue;
                    }
                    if (stack->GetCount() != expected.count ||
                        reinterpret_cast<std::uintptr_t>(stack) !=
                            expected.stackIdentity) {
                        continue;
                    }

                    RE::BSTSmartPointer<RE::TBO_InstanceData>
                        instanceData{};
                    if (stack->extra) {
                        if (const auto* instance =
                                stack->extra->GetByType<
                                    RE::ExtraInstanceData>()) {
                            instanceData = instance->data;
                        }
                    }
                    if (reinterpret_cast<std::uintptr_t>(
                            instanceData.get()) !=
                        expected.instanceIdentity) {
                        continue;
                    }

                    const bool equipped =
                        isEquippedWeaponStack(
                            player,
                            *weapon,
                            *stack,
                            stackCount);
                    if (equipped != expected.equipped) {
                        continue;
                    }
                    auto* equipSlot =
                        weapon->GetEquipSlot(instanceData.get());
                    if (!equipSlot) {
                        equipSlot = weapon->GetEquipSlot(nullptr);
                    }
                    return RuntimeWeaponTarget{
                        .weapon = weapon,
                        .instanceData = std::move(instanceData),
                        .equipSlot = equipSlot,
                        .stackId = stackId,
                        .equipped = equipped,
                    };
                }
            }
            return std::nullopt;
        }
    }

    Revision probeRevision() noexcept
    {
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->inventoryList) {
                return {};
            }

            RevisionBuilder revision;
            revision.addContext(
                player->race ? player->race->GetFormID() : 0,
                pipboySlotMask(*player));
            visitVisibleStacks(
                *player,
                [&revision](
                    const ClassifiedItem& classified,
                    const RE::BGSInventoryItem::Stack& stack,
                    const std::uint32_t stackId,
                    const std::uint32_t count,
                    const RE::TBO_InstanceData* instanceData,
                    const char* name,
                    const std::uint32_t armorSlotMask,
                    const std::uint32_t armorZoneFlags,
                    const bool equipped) {
                    revision.addRow(
                        classified.category,
                        classified.object->GetFormID(),
                        stackId,
                        count,
                        armorSlotMask,
                        armorZoneFlags,
                        equipped,
                        reinterpret_cast<std::uintptr_t>(&stack),
                        reinterpret_cast<std::uintptr_t>(
                            instanceData),
                        name);
                });
            return revision.finish(true);
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack failed to probe the player inventory ({}).",
                error.what());
        } catch (...) {
            logger::error(
                "ROCK Backpack failed to probe the player inventory.");
        }
        return {};
    }

    Snapshot capture(RE::TESObjectREFR& container) noexcept
    {
        try {
            Snapshot snapshot{};
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !container.inventoryList) {
                return snapshot;
            }

            snapshot.available = true;
            RevisionBuilder revision;
            revision.addContext(
                player->race ? player->race->GetFormID() : 0,
                pipboySlotMask(*player));
            visitVisibleStacks(
                *player,
                [&snapshot, &revision, player](
                    const ClassifiedItem& classified,
                    const RE::BGSInventoryItem::Stack& stack,
                    const std::uint32_t stackId,
                    const std::uint32_t count,
                    const RE::TBO_InstanceData* instanceData,
                    const char* name,
                    const std::uint32_t armorSlotMask,
                    const std::uint32_t armorZoneFlags,
                    const bool equipped) {
                    revision.addRow(
                        classified.category,
                        classified.object->GetFormID(),
                        stackId,
                        count,
                        armorSlotMask,
                        armorZoneFlags,
                        equipped,
                        reinterpret_cast<std::uintptr_t>(&stack),
                        reinterpret_cast<std::uintptr_t>(
                            instanceData),
                        name);
                    const auto unique = native_inventory::identity(stack.extra.get());
                    snapshot.rows.push_back(Row{
                        .name = name,
                        .armorSlotLabel =
                            classified.category == Category::Armor ?
                                armorSlotLabel(
                                    *player,
                                    armorSlotMask) :
                                std::string{},
                        .category = classified.category,
                        .formId = classified.object->GetFormID(),
                        .stackId = stackId,
                        .count = count,
                        .armorSlotMask = armorSlotMask,
                        .armorZoneFlags = armorZoneFlags,
                        .weight = resolvedWeight(
                            *classified.object,
                            instanceData),
                        .icon = resolveIcon(
                            classified,
                            instanceData,
                            armorZoneFlags),
                        .usable = isUsableCategory(classified),
                        .equipped = equipped,
                        .stackIdentity =
                            reinterpret_cast<std::uintptr_t>(&stack),
                        .instanceIdentity =
                            reinterpret_cast<std::uintptr_t>(
                                instanceData),
                        .uniqueBase = unique.base,
                        .uniqueIndex = unique.index,
                        .kind = storageKind(classified, instanceData),
                    });
                }, &container);
            snapshot.revision = revision.finish(true);
            std::sort(
                snapshot.rows.begin(),
                snapshot.rows.end(),
                rowLess);
            for (std::size_t index = 0;
                 index < snapshot.rows.size();
                 ++index) {
                auto& row = snapshot.rows[index];
                row.id = makeRowId(
                    row.category,
                    row.formId,
                    row.stackId,
                    index);
            }
            return snapshot;
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack failed to capture the player inventory ({}).",
                error.what());
        } catch (...) {
            logger::error(
                "ROCK Backpack failed to capture the player inventory.");
        }
        return {};
    }

    Snapshot capture() noexcept
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? capture(*const_cast<RE::PlayerCharacter*>(player)) : Snapshot{};
    }

    Row describe(RE::TESObjectREFR& reference) noexcept
    {
        Row row;
        const auto classified = classify(reference.GetObjectReference());
        if (!classified) return row;
        auto* extra = reference.extraList.get();
        const auto* instance = extra ? extra->GetByType<RE::ExtraInstanceData>() : nullptr;
        const auto* data = instance ? instance->data.get() : nullptr;
        row.formId = classified->object->formID;
        row.category = classified->category;
        if (const auto* name = reference.GetDisplayFullName()) row.name = name;
        if (row.name.empty()) row.name = classified->baseName ? classified->baseName : "Unnamed item";
        row.count = native_inventory::worldCount(reference);
        row.weight = resolvedWeight(*classified->object, data);
        row.usable = isUsableCategory(*classified);
        row.instanceIdentity = reinterpret_cast<std::uintptr_t>(data);
        row.kind = storageKind(*classified, data);
        return row;
    }

    Vitals captureVitals() noexcept
    {
        // A player can plausibly reach several thousand pounds of capacity
        // through perks and armor, but never this. A number beyond it means the
        // actor value we read is not the one we asked for, so the panel drops
        // the capacity readout instead of publishing a fabricated maximum.
        constexpr float kImplausibleCapacity = 1.0e6f;

        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return {};
            }

            Vitals vitals{};
            vitals.available = true;
            const float carried = player->GetWeightInContainer();
            vitals.carriedWeight =
                std::isfinite(carried) && carried > 0.0f ? carried : 0.0f;

            auto* actorValues = RE::ActorValue::GetSingleton();
            if (!actorValues) {
                return vitals;
            }

            /*
             * The ActorValue singleton's member offsets come from
             * CommonLibF4VR rather than from verified VR disassembly, so each
             * pointer is proven to be an AVIF form before it is dereferenced
             * and its editor id is logged once. A wrong offset then degrades
             * into a missing readout plus a diagnosable log line rather than a
             * crash or a silently wrong number.
             */
            const auto resolveActorValue =
                [](const RE::ActorValueInfo* info,
                   const char* label,
                   bool& logged) noexcept -> const RE::ActorValueInfo* {
                if (!info || !info->Is(RE::ENUM_FORM_ID::kAVIF)) {
                    return nullptr;
                }
                if (!logged) {
                    logged = true;
                    const char* editorId = info->GetFormEditorID();
                    logger::info(
                        "ROCK Backpack resolved the {} actor value as '{}' ({:08X}).",
                        label,
                        editorId && *editorId ? editorId : "<unnamed>",
                        info->GetFormID());
                }
                return info;
            };

            // These flags are game-task-only state; captureVitals is never
            // called from the ROCK provider callback or the scheduler thread.
            static bool loggedCarryWeight = false;
            static bool loggedHealth = false;

            if (const auto* carryWeight = resolveActorValue(
                    actorValues->carryWeight,
                    "carry-capacity",
                    loggedCarryWeight)) {
                const float capacity = player->GetActorValue(*carryWeight);
                if (std::isfinite(capacity) && capacity > 0.0f &&
                    capacity < kImplausibleCapacity) {
                    vitals.carryCapacity = capacity;
                    vitals.capacityKnown = true;
                }
            }

            if (const auto* health = resolveActorValue(
                    actorValues->health,
                    "health",
                    loggedHealth)) {
                const float current = player->GetActorValue(*health);
                const float permanent =
                    player->GetPermanentActorValue(*health);
                if (std::isfinite(current) && std::isfinite(permanent) &&
                    permanent > 0.0f && permanent < kImplausibleCapacity) {
                    vitals.health = current > 0.0f ? current : 0.0f;
                    vitals.maxHealth = permanent;
                }
            }
            return vitals;
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack failed to capture player vitals ({}).",
                error.what());
        } catch (...) {
            logger::error(
                "ROCK Backpack failed to capture player vitals.");
        }
        return {};
    }

    WeaponActionResult toggleWeapon(
        const Row& expected) noexcept
    {
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return WeaponActionResult::PlayerUnavailable;
            }
            if (!player->inventoryList) {
                return WeaponActionResult::InventoryUnavailable;
            }
            if (expected.category != Category::Weapons) {
                return WeaponActionResult::StackUnavailable;
            }

            auto target =
                findRuntimeWeaponTarget(*player, expected);
            if (!target || !target->weapon) {
                return WeaponActionResult::StackUnavailable;
            }
            if (!target->equipSlot) {
                return WeaponActionResult::EquipSlotUnavailable;
            }

            auto* equipManager =
                RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                return WeaponActionResult::EquipManagerUnavailable;
            }

            RE::BGSObjectInstance objectInstance(
                target->weapon,
                target->instanceData.get());
            const bool accepted = target->equipped ?
                                      equipManager->UnequipObject(
                                          player,
                                          &objectInstance,
                                          1,
                                          target->equipSlot,
                                          target->stackId,
                                          true,
                                          false,
                                          true,
                                          true,
                                          nullptr) :
                                      equipManager->EquipObject(
                                          player,
                                          objectInstance,
                                          target->stackId,
                                          1,
                                          target->equipSlot,
                                          true,
                                          false,
                                          true,
                                          true,
                                          false);
            if (!accepted) {
                return WeaponActionResult::NativeActionRejected;
            }
            return target->equipped ?
                       WeaponActionResult::Unequipped :
                       WeaponActionResult::Equipped;
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack weapon toggle failed ({}).",
                error.what());
        } catch (...) {
            logger::error(
                "ROCK Backpack weapon toggle failed.");
        }
        return WeaponActionResult::NativeActionRejected;
    }

    const char* weaponResultMessage(
        const WeaponActionResult result) noexcept
    {
        switch (result) {
        case WeaponActionResult::Equipped:
            return "Weapon equip accepted";
        case WeaponActionResult::Unequipped:
            return "Weapon unequip accepted";
        case WeaponActionResult::PlayerUnavailable:
            return "Player is not ready";
        case WeaponActionResult::InventoryUnavailable:
            return "Inventory is not ready";
        case WeaponActionResult::StackUnavailable:
            return "Weapon changed; inventory refreshed";
        case WeaponActionResult::EquipSlotUnavailable:
            return "Weapon has no usable equip slot";
        case WeaponActionResult::EquipManagerUnavailable:
            return "Equipment manager is not ready";
        case WeaponActionResult::NativeActionRejected:
        default:
            return "Game rejected the weapon request";
        }
    }

    ArmorActionResult setArmorEquipped(
        const Row& expected,
        const bool equipped) noexcept
    {
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return ArmorActionResult::PlayerUnavailable;
            }
            if (!player->inventoryList) {
                return ArmorActionResult::InventoryUnavailable;
            }
            if (expected.category != Category::Armor) {
                return ArmorActionResult::ProtectedArmor;
            }

            auto target = findRuntimeArmorTarget(*player, expected);
            if (!target || !target->armor) {
                return ArmorActionResult::StackUnavailable;
            }
            if (target->equipped == equipped) {
                return ArmorActionResult::AlreadyInState;
            }

            auto* equipManager =
                RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                return ArmorActionResult::EquipManagerUnavailable;
            }

            RE::BGSObjectInstance objectInstance(
                target->armor,
                target->instanceData.get());
            const bool accepted = equipped ?
                                      equipManager->EquipObject(
                                          player,
                                          objectInstance,
                                          target->stackId,
                                          1,
                                          target->equipSlot,
                                          true,
                                          false,
                                          true,
                                          true,
                                          false) :
                                      equipManager->UnequipObject(
                                          player,
                                          &objectInstance,
                                          1,
                                          target->equipSlot,
                                          target->stackId,
                                          true,
                                          false,
                                          true,
                                          true,
                                          nullptr);
            if (!accepted) {
                return ArmorActionResult::NativeActionRejected;
            }
            return equipped ?
                       ArmorActionResult::Equipped :
                       ArmorActionResult::Unequipped;
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack armor action failed ({}).",
                error.what());
        } catch (...) {
            logger::error("ROCK Backpack armor action failed.");
        }
        return ArmorActionResult::NativeActionRejected;
    }

    const char* armorResultMessage(
        const ArmorActionResult result) noexcept
    {
        switch (result) {
        case ArmorActionResult::Equipped:
            return "Armor equip accepted";
        case ArmorActionResult::Unequipped:
            return "Armor unequip accepted";
        case ArmorActionResult::AlreadyInState:
            return "Armor already matches that state";
        case ArmorActionResult::PlayerUnavailable:
            return "Player is not ready";
        case ArmorActionResult::InventoryUnavailable:
            return "Inventory is not ready";
        case ArmorActionResult::StackUnavailable:
            return "Armor changed; inventory refreshed";
        case ArmorActionResult::ProtectedArmor:
            return "That armor cannot be managed here";
        case ArmorActionResult::EquipManagerUnavailable:
            return "Equipment manager is not ready";
        case ArmorActionResult::NativeActionRejected:
        default:
            return "Game rejected the armor request";
        }
    }

    UseActionResult useItem(const Row& expected) noexcept
    {
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return UseActionResult::PlayerUnavailable;
            }
            if (!player->inventoryList) {
                return UseActionResult::InventoryUnavailable;
            }
            // Aid is consumed and notes are played; everything else has no
            // meaningful use. Weapons and armor have their own equip paths, and
            // letting a generic "use" reach junk or ammo would just be an equip
            // call the game rejects.
            if (expected.category != Category::Aid &&
                expected.category != Category::Misc) {
                return UseActionResult::NotConsumable;
            }

            auto target = findRuntimeItemTarget(*player, expected);
            if (!target || !target->object) {
                return UseActionResult::StackUnavailable;
            }

            // Notes go through the Pip-Boy's own holotape dispatch rather than
            // the equip manager: the engine dispatches on NOTE_TYPE behind it.
            if (auto* note = target->object->As<RE::BGSNote>()) {
                return playNote(*note) ?
                           UseActionResult::Used :
                           UseActionResult::NativeActionRejected;
            }

            auto* aid = target->object->As<RE::AlchemyItem>();
            if (!aid) {
                return UseActionResult::NotConsumable;
            }

            auto* equipManager = RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                return UseActionResult::EquipManagerUnavailable;
            }

            auto* equipSlot = aid->GetEquipSlot(target->instanceData.get());
            if (!equipSlot) {
                equipSlot = aid->GetEquipSlot(nullptr);
            }

            RE::BGSObjectInstance objectInstance(
                aid,
                target->instanceData.get());
            // Consumables are applied immediately rather than queued: the
            // effect should land on the frame the user pressed the button, not
            // after an animation-graph round trip that the panel cannot see.
            const bool accepted = equipManager->EquipObject(
                player,
                objectInstance,
                target->stackId,
                1,
                equipSlot,
                false,
                false,
                true,
                true,
                false);
            return accepted ?
                       UseActionResult::Used :
                       UseActionResult::NativeActionRejected;
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack use action failed ({}).",
                error.what());
        } catch (...) {
            logger::error("ROCK Backpack use action failed.");
        }
        return UseActionResult::NativeActionRejected;
    }

    const char* useResultMessage(const UseActionResult result) noexcept
    {
        switch (result) {
        case UseActionResult::Used:
            return "Item used";
        case UseActionResult::PlayerUnavailable:
            return "Player is not ready";
        case UseActionResult::InventoryUnavailable:
            return "Inventory is not ready";
        case UseActionResult::StackUnavailable:
            return "Item changed; inventory refreshed";
        case UseActionResult::NotConsumable:
            return "That item cannot be used";
        case UseActionResult::EquipManagerUnavailable:
            return "Equipment manager is not ready";
        case UseActionResult::NativeActionRejected:
        default:
            return "Game rejected the use request";
        }
    }

    MaterializeOutcome materializeRow(
        const Row& expected,
        const RE::NiPoint3& dropLocation) noexcept
    {
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return { .result =
                             MaterializeActionResult::PlayerUnavailable };
            }
            if (!player->inventoryList) {
                return { .result =
                             MaterializeActionResult::InventoryUnavailable };
            }

            /*
             * Selection completes under the inventory read lock inside
             * findRuntimeItemTarget; RemoveItem is called only after that
             * helper has returned and released the lock, matching the ordering
             * the backpack materialization already relies on.
             */
            const auto target = findRuntimeItemTarget(*player, expected);
            if (!target || !target->object) {
                return { .result =
                             MaterializeActionResult::StackUnavailable };
            }

            RE::TESObjectREFR::RemoveItemData removeData(
                target->object,
                1);
            removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
            removeData.dropLoc = &dropLocation;
            removeData.stackData.push_back(target->stackId);

            const RE::ObjectRefHandle handle =
                player->RemoveItem(removeData);
            if (!handle) {
                return { .result =
                             MaterializeActionResult::NativeActionRejected };
            }
            return {
                .result = MaterializeActionResult::Materialized,
                .handle = handle,
            };
        } catch (const std::exception& error) {
            logger::error(
                "ROCK Backpack materialization failed ({}).",
                error.what());
        } catch (...) {
            logger::error("ROCK Backpack materialization failed.");
        }
        return { .result = MaterializeActionResult::NativeActionRejected };
    }

    const char* materializeResultMessage(
        const MaterializeActionResult result) noexcept
    {
        switch (result) {
        case MaterializeActionResult::Materialized:
            return "Item released into the world";
        case MaterializeActionResult::PlayerUnavailable:
            return "Player is not ready";
        case MaterializeActionResult::InventoryUnavailable:
            return "Inventory is not ready";
        case MaterializeActionResult::StackUnavailable:
            return "Item changed; inventory refreshed";
        case MaterializeActionResult::NativeActionRejected:
        default:
            return "Game rejected the release request";
        }
    }
}
