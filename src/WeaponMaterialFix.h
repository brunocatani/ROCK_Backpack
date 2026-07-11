#pragma once
#include <VRVanillaFixes/WeaponScenes.h>
#include <cstdint>

namespace RE { class NiAVObject; }
namespace rock_backpack::weapon_material_fix {
    // Each display owner keeps its own readiness. No world/equipped cull state
    // or scene pointer is shared with VR Vanilla Fixes between calls.
    class Preparation {
    public:
        bool update(RE::NiAVObject* root, std::uint32_t form);
    private:
        std::uint64_t _next{}, _unavailableSince{};
        bool _done{};
    };
    void referenceChanged(std::uint32_t reference);
}
