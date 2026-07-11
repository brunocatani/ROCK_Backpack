#pragma once
#include "RockServices.h"

namespace rock_backpack::backpack_return
{
    enum class Decision { Wait, Held, Return };
    // Only a coherent, free-hand publication authorizes parking. Neither a
    // missed release event nor throw velocity changes ownership of this item.
    inline Decision evaluate(std::uint32_t reference, bool retrieving, const RockFrame& frame)
    {
        if (!reference || retrieving || !frame.handsValid || !frame.providerReady || frame.menuBlocking || frame.configBlocking)
            return Decision::Wait;
        if (frame.leftHeldFormId == reference || frame.rightHeldFormId == reference) return Decision::Held;
        if ((frame.leftHolding && !frame.leftHeldFormId) || (frame.rightHolding && !frame.rightHeldFormId)) return Decision::Wait;
        return Decision::Return;
    }
}
