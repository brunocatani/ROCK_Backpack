#pragma once
#include "StorageGrid.h"
#include <cstdint>

namespace rock_backpack
{
    // Copied presentation identity. Numeric witnesses are comparison-only and
    // must be recaptured through Weapon 1.1 before using an attached display.
    struct InventoryDisplayItem {
        storage::ItemId id{};
        std::uint64_t appearance{},stackWitness{},instanceWitness{};
        std::uint32_t form{},index{},count{};
        storage::Kind kind{storage::Kind::Misc};
        explicit operator bool() const { return id && appearance && form && count==1; }
    };
}
