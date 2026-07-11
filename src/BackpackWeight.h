#pragma once
#include <cstdint>
namespace RE { class TESObjectREFR; }
namespace rock_backpack::backpack_weight
{
    bool install();
    bool available() noexcept;
    float container(RE::TESObjectREFR&);
    float reference(RE::TESObjectREFR&);
    void publish(float weight) noexcept;
}
