#pragma once
#include "PanelPoseMath.h"
#include <cstdint>

namespace rock_backpack::drop_policy
{
    using panel_math::Vec3;
    // The panel's Drop action places outside the hand's immediate grab/contact
    // area. It never uses the hidden storage container as a spatial anchor.
    inline std::optional<Vec3> nearHand(Vec3 hand,Vec3 head,Vec3 forward,Vec3 player)
    {
        if(!panel_math::isFinite(hand) || !panel_math::isFinite(head) ||
            !panel_math::isFinite(forward) || !panel_math::isFinite(player)) return {};
        const auto separation=hand-head;
        if(panel_math::dot(separation,separation)>180.f*180.f) return {};
        const auto horizontal=panel_math::normalize({forward.x,forward.y,0});
        auto target=hand+horizontal.value_or(Vec3{})*20.f;
        target.z=std::max(hand.z-20.f,player.z+12.f);
        return target;
    }
    enum class Placement { Wait, Confirmed, Rejected };
    inline Placement verify(std::uint64_t frame,std::uint64_t positionedFrame,
        Vec3 actual,Vec3 expected,bool sameCell)
    {
        // Several F4SE tasks can run before the next physics publication.
        if(frame<=positionedFrame) return Placement::Wait;
        if(!sameCell || !panel_math::isFinite(actual) || !panel_math::isFinite(expected)) return Placement::Rejected;
        const auto delta=actual-expected;
        return panel_math::dot(delta,delta)<=4.f*4.f?Placement::Confirmed:Placement::Rejected;
    }
}
