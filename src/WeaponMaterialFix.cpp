#include "PCH.h"
#include "WeaponMaterialFix.h"
#include <Windows.h>

namespace rock_backpack::weapon_material_fix {
    namespace {
        const vvf::api::WeaponScenesV1* api()
        {
            // All plugin modules are loaded before any display is created.
            // This integration remains optional when VVF is absent/older.
            static const auto* value = [] {
                const auto module = GetModuleHandleW(L"VRVanillaFixes.dll");
                const auto get = module ? reinterpret_cast<vvf::api::GetWeaponScenes>(
                    GetProcAddress(module, "VVF_GetWeaponScenes")) : nullptr;
                const auto* result = get ? get(1, 0, sizeof(vvf::api::WeaponScenesV1)) : nullptr;
                if (result && result->major == 1 && result->size >= sizeof(*result) &&
                    result->prepareDisplay && result->referenceChanged) {
                    logger::info("Backpack bound VR Vanilla Fixes weapon scenes {}.{}", result->major, result->minor);
                    return result;
                }
                logger::info("Backpack weapon scenes: VR Vanilla Fixes integration unavailable");
                return static_cast<const vvf::api::WeaponScenesV1*>(nullptr);
            }();
            return value;
        }
    }
    bool Preparation::update(RE::NiAVObject* root, std::uint32_t form)
    {
        if (_done) return true;
        const auto now = GetTickCount64();
        if (now < _next || !root) return false;
        const auto* provider = api();
        if (!provider) { _done = true; return true; }
        const auto result = provider->prepareDisplay(root);
        if (result.status == vvf::api::SceneStatus::TexturesPending ||
            result.status == vvf::api::SceneStatus::MaterialUnavailable) {
            _next = now + 100;
            if (result.status == vvf::api::SceneStatus::TexturesPending) { _unavailableSince = 0; return false; }
            if (!_unavailableSince) _unavailableSince = now;
            if (now - _unavailableSince < 5000) return false;
            logger::warn("Backpack weapon display material unavailable after construction: form={:08X} root={:X}",
                form, reinterpret_cast<std::uintptr_t>(root));
        }
        _done = true;
        logger::info("Backpack weapon display culling: form={:08X} root={:X} name='{}' status={} shapes={} hidden={}",
            form, reinterpret_cast<std::uintptr_t>(root), root->name.c_str(),
            unsigned(result.status), result.inspected, result.culled);
        return true;
    }
    void referenceChanged(std::uint32_t reference)
    {
        if (const auto* provider = api()) provider->referenceChanged(reference);
    }
}
