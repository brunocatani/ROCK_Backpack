#pragma once

#include <ROCK/Client.h>
#include <ROCK/Grab.h>
#include <ROCK/Hands.h>
#include <ROCK/Input.h>
#include <ROCK/Weapon.h>
#include <ROCK/Storage.h>
#include <ROCK/Animation.h>
#include <ROCK/Touch.h>
#include <array>

namespace rock_backpack
{
    // Addon-owned values joined from one provider publication, never an ABI record.
    struct RockFrame : rock::api::core::Snapshot
    {
        bool handsValid{};
        bool headValid{};
        bool leftHolding{ true };
        bool rightHolding{ true };
        std::uint32_t leftHeldFormId{}, rightHeldFormId{};
        rock::api::Transform leftHandTransform{};
        rock::api::Transform rightHandTransform{};
        rock::api::hands::HeadPose head{};
        std::array<rock::api::grab::HandInteractionState, 2> handStates{}; // right, left
        std::uint32_t equippedWeapon{};
    };

    struct RockServices
    {
        rock::api::Client client;
        const rock::api::grab::Api* grab{};
        const rock::api::hands::Api* hands{};
        const rock::api::input::Api* input{};
        const rock::api::weapon::Api* weapon{};
        const rock::api::storage::Api* storage{};
        const rock::api::animation::Api* animation{};
        const rock::api::touch::Api* authoredTouch{};
        // Written only by the ROCK frame callback after binding.
        std::uint32_t registeredPoseForm{};
        rock::api::Status lastPoseResult{rock::api::Status::NotReady};

        bool connect() noexcept;
        bool bind(rock::api::QueryInterfaceV1 query) noexcept;
        bool ensureBackpackPoses(std::uint32_t baseFormId) noexcept;
        RockFrame sample(const rock::api::core::Snapshot& core) const noexcept;
    };
}
