#pragma once
#include <cstdint>
namespace rock_backpack::storage
{
    struct DeliveryGesture {
        std::uint32_t reference{};
        bool neutral{};
        void delivered(std::uint32_t value) { reference = value; neutral = false; }
        bool permits(std::uint32_t value, bool available, bool down, bool /*pressed*/)
        {
            if (!reference || reference != value) return true;
            if (!available) return false;
            // Ignore only the release belonging to the draw. Once neutral was
            // observed, normal interaction resumes; do not keep a weapon behind
            // a persistent "fresh press" gate while it moves between slots.
            if(neutral) { reference=0; neutral=false; return true; }
            if(!down) neutral=true;
            return false;
        }
    };
}
