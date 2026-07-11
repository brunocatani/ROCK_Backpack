#pragma once
#include "PanelPoseMath.h"
#include <cmath>
#include <optional>

// Stash and draw transitions of mounted weapon visuals, as VirtualHolsters
// eases its holstered clones: a weapon put on a mount eases from where the
// hand let it go into the mount pose, and a drawn weapon's mount clone moves
// into the hand, eases into ROCK's firing aim and stands in for the real
// weapon until ROCK shows it. Transforms use the engine's stored-row
// convention (rows are the local axes in parent space; world = local*parent).
namespace rock_backpack::weapon_transition
{
    using panel_math::StoredTransform;
    using panel_math::Vec3;

    inline constexpr float kStashSeconds = .18f;    // hand (or a failed draw) -> mount
    inline constexpr float kDrawSeconds = .12f;     // mount clone -> firing aim
    // A stash arrival older than this when its mount visual appears no longer
    // starts from the hand: the visual simply appears on the mount.
    inline constexpr float kArrivalLifetimeSeconds = 1.f;
    // After ROCK accepts the equip, the stand-in waits at most this long for
    // ROCK to show the real weapon before it is removed.
    inline constexpr float kRevealWaitSeconds = .4f;
    // A stand-in whose draw never reports a result is put away after this.
    inline constexpr float kStandInLimitSeconds = 3.f;

    [[nodiscard]] inline float smoothstep(float t) noexcept
    {
        t = std::isfinite(t) ? (std::min)(1.f, (std::max)(0.f, t)) : 1.f;
        return t * t * (3.f - 2.f * t);
    }

    // The local transform that places a child of `parent` at `world`.
    [[nodiscard]] inline std::optional<StoredTransform> localPose(const StoredTransform& parent, const StoredTransform& world)
    {
        if (!std::isfinite(parent.scale) || std::fabs(parent.scale) <= .0001f || !panel_math::isFinite(parent.translation) ||
            !panel_math::isFinite(world.translation) || !std::isfinite(world.scale)) return {};
        for (const auto v : parent.rotation) if (!std::isfinite(v)) return {};
        for (const auto v : world.rotation) if (!std::isfinite(v)) return {};
        const auto inParent = [&](Vec3 value) {
            return Vec3{panel_math::dot(value, {parent.rotation[0], parent.rotation[1], parent.rotation[2]}),
                panel_math::dot(value, {parent.rotation[3], parent.rotation[4], parent.rotation[5]}),
                panel_math::dot(value, {parent.rotation[6], parent.rotation[7], parent.rotation[8]})};
        };
        StoredTransform result;
        result.translation = inParent(world.translation - parent.translation) * (1.f / parent.scale);
        for (unsigned row = 0; row < 3; ++row) {
            const auto axis = inParent({world.rotation[row * 3], world.rotation[row * 3 + 1], world.rotation[row * 3 + 2]});
            result.rotation[row * 3] = axis.x; result.rotation[row * 3 + 1] = axis.y; result.rotation[row * 3 + 2] = axis.z;
        }
        result.scale = world.scale / parent.scale;
        return panel_math::isFinite(result.translation) && std::isfinite(result.scale) ? std::optional{result} : std::nullopt;
    }

    namespace detail
    {
        struct Quaternion { float w{1}, x{}, y{}, z{}; };
        [[nodiscard]] inline Quaternion toQuaternion(const std::array<float, 9>& m) noexcept
        {
            const auto r = [&](unsigned row, unsigned column) { return m[row * 3 + column]; };
            Quaternion q;
            const float trace = r(0, 0) + r(1, 1) + r(2, 2);
            if (trace > 0) {
                const float s = std::sqrt(trace + 1.f) * 2.f;
                q = {.25f * s, (r(2, 1) - r(1, 2)) / s, (r(0, 2) - r(2, 0)) / s, (r(1, 0) - r(0, 1)) / s};
            } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
                const float s = std::sqrt(1.f + r(0, 0) - r(1, 1) - r(2, 2)) * 2.f;
                q = {(r(2, 1) - r(1, 2)) / s, .25f * s, (r(0, 1) + r(1, 0)) / s, (r(0, 2) + r(2, 0)) / s};
            } else if (r(1, 1) > r(2, 2)) {
                const float s = std::sqrt(1.f + r(1, 1) - r(0, 0) - r(2, 2)) * 2.f;
                q = {(r(0, 2) - r(2, 0)) / s, (r(0, 1) + r(1, 0)) / s, .25f * s, (r(1, 2) + r(2, 1)) / s};
            } else {
                const float s = std::sqrt(1.f + r(2, 2) - r(0, 0) - r(1, 1)) * 2.f;
                q = {(r(1, 0) - r(0, 1)) / s, (r(0, 2) + r(2, 0)) / s, (r(1, 2) + r(2, 1)) / s, .25f * s};
            }
            return q;
        }
        [[nodiscard]] inline std::array<float, 9> fromQuaternion(const Quaternion& q) noexcept
        {
            return {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w),
                2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w),
                2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)};
        }
    }

    // Rotation slerps (shortest arc), position and scale interpolate.
    [[nodiscard]] inline StoredTransform blend(const StoredTransform& from, const StoredTransform& to, float t) noexcept
    {
        using detail::Quaternion;
        if (t <= 0) return from;
        if (t >= 1) return to;
        const auto a = detail::toQuaternion(from.rotation);
        auto b = detail::toQuaternion(to.rotation);
        float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (cosine < 0) { b = {-b.w, -b.x, -b.y, -b.z}; cosine = -cosine; }
        Quaternion q;
        if (cosine > .9995f) {
            q = {a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        } else {
            const float theta = std::acos(cosine), sine = std::sin(theta);
            const float wa = std::sin((1 - t) * theta) / sine, wb = std::sin(t * theta) / sine;
            q = {a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb};
        }
        const float length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (!(length > .0001f) || !std::isfinite(length)) return to;
        q = {q.w / length, q.x / length, q.y / length, q.z / length};
        StoredTransform result;
        result.rotation = detail::fromQuaternion(q);
        result.translation = from.translation + (to.translation - from.translation) * t;
        result.scale = from.scale + (to.scale - from.scale) * t;
        return result;
    }

    // One eased move of a visual's local transform. The target may move while
    // it runs (a stand-in following the hand's firing aim).
    struct Ease
    {
        StoredTransform from{};
        float elapsed{}, duration{};
        bool active{};
        void start(const StoredTransform& start, float seconds) noexcept { from = start; elapsed = 0; duration = seconds; active = seconds > 0; }
        // Advances by `seconds` and returns the pose toward `target`.
        StoredTransform advance(const StoredTransform& target, float seconds) noexcept
        {
            if (!active) return target;
            elapsed += std::isfinite(seconds) && seconds > 0 ? seconds : 0;
            const float linear = duration > 0 ? elapsed / duration : 1.f;
            if (linear >= 1) { active = false; return target; }
            return blend(from, target, smoothstep(linear));
        }
    };
}
