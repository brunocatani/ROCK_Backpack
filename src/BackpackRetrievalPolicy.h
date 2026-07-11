#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace rock_backpack::retrieval_policy
{
    struct Vec3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    [[nodiscard]] inline constexpr Vec3 operator+(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return { left.x + right.x, left.y + right.y, left.z + right.z };
    }

    [[nodiscard]] inline constexpr Vec3 operator-(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return { left.x - right.x, left.y - right.y, left.z - right.z };
    }

    [[nodiscard]] inline constexpr Vec3 operator*(
        const Vec3& value,
        const float scale) noexcept
    {
        return { value.x * scale, value.y * scale, value.z * scale };
    }

    [[nodiscard]] inline constexpr float dot(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    [[nodiscard]] inline constexpr Vec3 cross(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x,
        };
    }

    [[nodiscard]] inline bool finite(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    [[nodiscard]] inline bool normalize(
        const Vec3& value,
        Vec3& output) noexcept
    {
        if (!finite(value)) {
            return false;
        }
        const float lengthSquared = dot(value, value);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-6f) {
            return false;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        output = value * inverseLength;
        return finite(output);
    }

    // The geometric sampler takes a horizontal anchor; live retrieval uses
    // headAnchor below, never the independently turning pelvis or head pitch.
    // The left shoulder belongs exclusively to backpack retrieval/storage.
    inline constexpr Vec3 kBackpackOffset{-14.f,-18.f,0};
    inline constexpr float kEnterRadius=13.f,kExitRadius=15.f,kMinimumBehind=4.f;

    struct ZoneSample
    {
        bool valid{ false };
        bool withinEnter{ false };
        bool withinExit{ false };
    };

    [[nodiscard]] inline ZoneSample sampleBackZone(
        const Vec3& handPosition,
        const Vec3& bodyPosition,
        const Vec3& bodyForward) noexcept
    {
        constexpr Vec3 worldUp{ 0.0f, 0.0f, 1.0f };
        if (!finite(handPosition) || !finite(bodyPosition)) {
            return {};
        }

        Vec3 forward{};
        Vec3 right{};
        if (!normalize({bodyForward.x,bodyForward.y,0}, forward) ||
            !normalize(cross(forward, worldUp), right)) {
            return {};
        }

        const Vec3 fromBody = handPosition - bodyPosition;
        const bool behind =
            dot(fromBody, forward) <= -kMinimumBehind;
        const Vec3 leftCenter =
            bodyPosition +
            right * kBackpackOffset.x +
            forward * kBackpackOffset.y +
            worldUp * kBackpackOffset.z;
        const auto distanceSquared = [&handPosition](const Vec3& center) {
            const Vec3 delta = handPosition - center;
            return dot(delta, delta);
        };
        const float leftSquared = distanceSquared(leftCenter);
        if (!std::isfinite(leftSquared)) {
            return {};
        }

        const float nearestSquared = leftSquared;
        return {
            .valid = true,
            .withinEnter =
                behind &&
                nearestSquared <=
                    kEnterRadius * kEnterRadius,
            .withinExit =
                behind &&
                nearestSquared <=
                    kExitRadius * kExitRadius,
        };
    }

    struct HeadAnchor { Vec3 position,forward; };
    inline std::optional<HeadAnchor> headAnchor(Vec3 head,Vec3 forward,Vec3 right) noexcept
    {
        if(!finite(head) || !finite(forward) || !finite(right)) return {};
        Vec3 yawRight{},yawForward{};
        // The HMD right axis remains usable while looking straight down/up.
        // With extreme head roll use the forward projection instead.
        if(right.x*right.x+right.y*right.y>.25f && normalize({right.x,right.y,0},yawRight))
            yawForward={-yawRight.y,yawRight.x,0};
        else if(forward.x*forward.x+forward.y*forward.y>.25f && normalize({forward.x,forward.y,0},yawForward))
            yawRight={yawForward.y,-yawForward.x,0};
        else return {};
        return HeadAnchor{head+Vec3{0,0,-6.85f},yawForward};
    }
    inline Vec3 leftCenter(const HeadAnchor& anchor) noexcept
    {
        const auto right=cross(anchor.forward,Vec3{0,0,1});
        return anchor.position+right*kBackpackOffset.x+anchor.forward*kBackpackOffset.y;
    }
    inline ZoneSample sampleHeadBackZone(Vec3 hand,Vec3 head,Vec3 forward,Vec3 right) noexcept
    {
        const auto anchor=headAnchor(head,forward,right);
        return anchor?sampleBackZone(hand,anchor->position,anchor->forward):ZoneSample{};
    }

    enum class Phase : std::uint8_t
    {
        NeedOutsideNeutral,
        ReadyOutside,
        InsideNeutral,
        Armed,
        AwaitReleaseOutside,
    };

    enum class Outcome : std::uint8_t
    {
        None,
        PullCommitted,
    };

    struct State
    {
        Phase phase{ Phase::NeedOutsideNeutral };
    };

    struct Input
    {
        bool eligible{ false };
        bool rawGripAvailable{ false };
        bool gripHeld{ false };
        ZoneSample zone{};
    };
    // Ownership must survive the exit frame: that is the frame that commits
    // the pull, and can also touch a weapon collider or another body zone.
    inline bool reservesHand(const State& state,const Input& input)
    {
        return input.eligible && input.rawGripAvailable && input.zone.valid &&
            (input.zone.withinExit || (state.phase==Phase::Armed && input.gripHeld));
    }
    enum class Prompt { Hidden, Grip, Pull, Rearm };
    inline Prompt prompt(const State& state,bool eligible,bool raw,const ZoneSample& zone)
    {
        if(!eligible || !raw || !zone.valid || !zone.withinExit) return Prompt::Hidden;
        return state.phase==Phase::Armed?Prompt::Pull:state.phase==Phase::InsideNeutral?Prompt::Grip:Prompt::Rearm;
    }

    inline void reset(State& state) noexcept
    {
        state = {};
    }

    [[nodiscard]] inline Outcome advance(
        State& state,
        const Input& input) noexcept
    {
        if (!input.eligible || !input.rawGripAvailable || !input.zone.valid) {
            reset(state);
            return Outcome::None;
        }

        switch (state.phase) {
        case Phase::NeedOutsideNeutral:
            if (!input.gripHeld) {
                if(input.zone.withinEnter) state.phase=Phase::InsideNeutral;
                else if(!input.zone.withinExit) state.phase=Phase::ReadyOutside;
            }
            break;
        case Phase::ReadyOutside:
            if(input.zone.withinEnter) {
                // Previous sample was neutral: a press on the entry frame is
                // a fresh grip, not a grip carried into the zone from outside.
                state.phase=input.gripHeld?Phase::Armed:Phase::InsideNeutral;
            } else if (input.gripHeld) {
                state.phase = Phase::NeedOutsideNeutral;
            }
            break;
        case Phase::InsideNeutral:
            if(!input.zone.withinExit) {
                state.phase=input.gripHeld?Phase::NeedOutsideNeutral:Phase::ReadyOutside;
            } else if (input.gripHeld) {
                state.phase = Phase::Armed;
            }
            break;
        case Phase::Armed:
            if (!input.gripHeld) {
                state.phase = Phase::NeedOutsideNeutral;
            } else if (!input.zone.withinExit) {
                state.phase = Phase::AwaitReleaseOutside;
                return Outcome::PullCommitted;
            }
            break;
        case Phase::AwaitReleaseOutside:
            if (!input.gripHeld && !input.zone.withinExit) {
                state.phase = Phase::ReadyOutside;
            }
            break;
        }
        return Outcome::None;
    }
}
