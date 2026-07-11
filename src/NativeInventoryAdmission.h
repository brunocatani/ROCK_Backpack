#pragma once
#include <ROCK/Weapon.h>
namespace rock_backpack::native_admission
{
    bool install();
    // ROCK's Weapon table identifies equip pickups for Backpack's owner. Set on
    // the game thread when Backpack binds ROCK and cleared before that owner
    // closes; until then every native pickup goes through capacity admission.
    void bindRock(const rock::api::weapon::Api* weapon, rock::api::OwnerToken owner) noexcept;
    void unbindRock() noexcept;
    void refused(bool bulk=false,bool stowing=false) noexcept;
}
