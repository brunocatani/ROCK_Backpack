#pragma once

#include "RE/Bethesda/BSPointerHandle.h"

#include <cstdint>

namespace RE
{
    class NiPoint3;
}

namespace rock_backpack::backpack_item
{
    struct MaterializeResult
    {
        bool success{ false };
        RE::ObjectRefHandle handle{};
        const char* reason{ "not-attempted" };
    };

    [[nodiscard]] std::uint32_t inventoryCount(
        std::uint32_t baseFormId) noexcept;

    [[nodiscard]] MaterializeResult materializeFromInventory(
        std::uint32_t baseFormId,
        const RE::NiPoint3& dropLocation) noexcept;

}
