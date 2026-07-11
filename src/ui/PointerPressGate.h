#pragma once
#include "PanelModel.h"
namespace rock_backpack::ui
{
    struct PointerPressGate
    {
        std::uint64_t presentation{};
        Hand hand{ Hand::None };
        bool armed{};
        bool update(std::uint64_t epoch, Hand pointer, bool down) noexcept
        {
            if (presentation != epoch || hand != pointer || pointer == Hand::None) armed = false;
            presentation = epoch; hand = pointer;
            if (pointer != Hand::None && !down) armed = true;
            return armed && down;
        }
    };
}
