#pragma once
#include <ROCK/Grab.h>
#include <ROCK/Input.h>
#include "StorageGrid.h"

namespace rock_backpack::interaction_policy
{
    enum class Runtime { Waiting, Regular, Rejected };
    constexpr Runtime runtime(bool regularLoaded, bool v2Loaded)
    { return v2Loaded ? Runtime::Rejected : regularLoaded ? Runtime::Regular : Runtime::Waiting; }

    constexpr bool equipsOnDraw(storage::Kind kind)
    { return storage::isWeapon(kind); }

    constexpr bool carriesEquipped(std::uint32_t flags, std::uint32_t target, std::uint32_t equipped,std::uint32_t previousStash=0)
    {
        using Flag = rock::api::grab::HandInteractionFlag;
        constexpr auto carrying = unsigned(Flag::NativeWeaponCarry) | unsigned(Flag::FiringGrip) | unsigned(Flag::PartCarry);
        constexpr auto loose = unsigned(Flag::LooseObject) | unsigned(Flag::LooseWeapon);
        // An active authored firing grip replaces NativeWeaponCarry in ROCK's
        // publication. Support-only PartGrip does not own the equipped item.
        // Our input suppression can retire FiringGrip before the raw release
        // reaches this consumer. Preserve only a previously identified same-item
        // stash across that suppressed transition; never infer support-hand ownership.
        const bool continuing=previousStash && previousStash==equipped &&
            (flags & unsigned(Flag::InputSuppressed)) && !(flags & unsigned(Flag::PartGrip));
        return equipped && target == equipped && ((flags & carrying) || continuing) && !(flags & loose);
    }
    constexpr std::uint32_t zoneInputFlags(bool deposit, bool mounted)
    {
        using Flag = rock::api::input::HandInputSuppressionFlag;
        return unsigned(Flag::SuppressGameplayCandidates) |
            (deposit ? unsigned(Flag::SuppressGrabRelease) | unsigned(Flag::SuppressNormalGrabPress) |
                unsigned(Flag::SuppressHeldWeaponTriggerEquip) : 0) |
            (mounted ? unsigned(Flag::SuppressNormalGrabPress) : 0);
    }
    struct DepositDecision { bool feedback{}, commit{}; };
    constexpr DepositDecision depositDecision(bool ready, bool releaseEdge, bool deliveryAllowsRelease)
    {
        // Feedback does not depend on the current grab level or a fresh press.
        // Only the draw's own initial release is excluded from committing.
        return {ready, ready && releaseEdge && deliveryAllowsRelease};
    }
}
