#pragma once
#include <RE/Bethesda/BSExtraData.h>
#include <type_traits>

namespace rock_backpack::native_inventory
{
    void releaseExtra(RE::ExtraDataList*) noexcept;
    template<class T> struct ExtraListRefManager {
        static_assert(std::is_same_v<T,RE::ExtraDataList>);
        static void Acquire(T* value) noexcept { value->IncRef(); }
        static void Release(T* value) noexcept { releaseExtra(value); }
    };
    // Native intrusive ownership, including the game's destructor when this
    // lease outlives the stack. CommonLib's nonvirtual C++ destructor does not
    // destroy the native extra-data chain.
    using ExtraLease=RE::BSTSmartPointer<RE::ExtraDataList,ExtraListRefManager>;
}
