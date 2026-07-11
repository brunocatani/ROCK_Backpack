#include "PCH.h"
#include "BackpackWeight.h"
#include <RE/Bethesda/BGSInventoryItem.h>
#include <F4SE/Trampoline.h>
#include <RPS/Runtime/HookChain.h>
#include <atomic>
#include <cstring>

namespace rock_backpack::backpack_weight
{
    namespace
    {
        // FO4VR 1.2.72: 3E8770 is called by both the encumbrance predicate
        // DD7CE0 and the current/max readout F03430. Both consume XMM0.
        constexpr std::array<std::uint8_t, 9> prologue{0x40,0x57,0x48,0x83,0xEC,0x60,0x48,0x8B,0xF9};
        constexpr std::array<std::uint8_t, 9> calcPrologue{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x02};
        using GetWeight = float(*)(RE::TESObjectREFR*);
        GetWeight original{};
        std::atomic<float> carried{};
        static_assert(std::atomic<float>::is_always_lock_free);
        // Constructor witness 3E87AE..CE; independent readers/writers in
        // 412390 and 409A70 establish owner +10, object +18, subtotal +20.
        struct Calculator {
            void* vtable{};
            float total{}; std::uint32_t padding{};
            RE::TESObjectREFR* owner{};
            RE::TESBoundObject* object{};
            float subtotal{}; std::uint32_t tail{};
        };
        static_assert(sizeof(Calculator) == 0x28 && offsetof(Calculator, owner) == 0x10 && offsetof(Calculator, subtotal) == 0x20);
        float calculate(const void* item)
        {
            Calculator calculator; calculator.owner = RE::PlayerCharacter::GetSingleton();
            if (!calculator.owner) return 0;
            // Native calculator applies instance weight, quest exclusions and
            // the player's AdjustItemWeight perks, including whole-stack rules.
            const auto result = reinterpret_cast<float(*)(Calculator*, const void*)>(REL::Module::get().base() + 0x412390)(&calculator, item);
            return std::isfinite(result) && result > 0 ? result : 0;
        }
        float hooked(RE::TESObjectREFR* ref) noexcept
        {
            const float native = original(ref);
            return ref == RE::PlayerCharacter::GetSingleton() ? native + carried.load(std::memory_order_relaxed) : native;
        }
    }
    bool available() noexcept { return original != nullptr; }
    bool install()
    {
        if (original) return true;
        const auto base = REL::Module::get().base();
        const auto entry = RPS::Runtime::HookChain::resolveEntry(base + 0x3E8770, prologue);
        if (!entry || !RPS::Runtime::HookChain::resolveEntry(base + 0x412390, calcPrologue)) {
            logger::error("Backpack carried-weight native guard failed"); return false;
        }
        std::array<std::uint8_t, 6> before{};
        std::memcpy(before.data(), reinterpret_cast<const void*>(base + 0x3E8770), before.size());
        try {
            static F4SE::Trampoline trampoline;
            trampoline.create(64);
            if (entry.foreign()) {
                // Another plugin's jump replaces the prologue: its detour is the
                // original, and only that jump is replaced.
                logger::info("Backpack carried weight chains behind {}", entry.owner);
                original = reinterpret_cast<GetWeight>(entry.callee);
                trampoline.write_branch<5>(base + 0x3E8770, hooked);
                return true;
            }
            auto* entry = static_cast<std::uint8_t*>(trampoline.allocate(20));
            // The first six bytes are complete position-independent instructions.
            std::memcpy(entry, prologue.data(), 6);
            constexpr std::array<std::uint8_t, 6> jump{0xFF,0x25,0,0,0,0};
            std::memcpy(entry + 6, jump.data(), jump.size());
            const auto continuation = base + 0x3E8776;
            std::memcpy(entry + 12, &continuation, 8);
            original = reinterpret_cast<GetWeight>(entry);
            trampoline.write_branch<5>(base + 0x3E8770, hooked);
            REL::safe_write<std::uint8_t>(base + 0x3E8775, 0x90);
            return true;
        } catch (...) {
            REL::safe_write(base + 0x3E8770, before.data(), before.size());
            original = nullptr; return false;
        }
    }
    float container(RE::TESObjectREFR& ref)
    {
        if (!original || !ref.inventoryList) return 0;
        const RE::BSAutoReadLock lock(ref.inventoryList->rwLock);
        float total{};
        for (const auto& item : ref.inventoryList->data) if (item.object) total += calculate(&item);
        return total;
    }
    float reference(RE::TESObjectREFR& ref)
    {
        if (!original || !ref.GetObjectReference()) return 0;
        struct StackView {
            std::uintptr_t vtable{};
            std::uint32_t references{1}, padding{};
            void* next{};
            RE::ExtraDataList* extra{};
            std::uint32_t count{1};
            std::uint16_t flags{}, tail{};
        } stack;
        static_assert(sizeof(StackView) == sizeof(RE::BGSInventoryItem::Stack) && offsetof(StackView, extra) == 0x18 && offsetof(StackView, count) == 0x20);
        stack.vtable = REL::Relocation<std::uintptr_t>{RE::BGSInventoryItem::Stack::VTABLE[0]}.address();
        stack.extra = ref.extraList.get();
        // Stack is borrowed for this call. Do not construct a smart pointer
        // which would attempt to free this automatic object.
        struct ItemView { RE::TESBoundObject* object; const StackView* stack; } view{ref.GetObjectReference(), &stack};
        static_assert(sizeof(ItemView) == sizeof(RE::BGSInventoryItem));
        return calculate(&view);
    }
    void publish(float weight) noexcept
    { carried.store(std::isfinite(weight) && weight > 0 ? weight : 0, std::memory_order_relaxed); }
}
