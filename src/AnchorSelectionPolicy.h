#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace rock_backpack::anchor_policy
{
    struct Candidate
    {
        std::uint32_t formId{ 0 };
        float distanceSquared{ std::numeric_limits<float>::infinity() };
        bool eligible{ false };
    };

    struct Selection
    {
        std::uint32_t formId{ 0 };
        float distanceSquared{ std::numeric_limits<float>::infinity() };

        [[nodiscard]] explicit operator bool() const noexcept { return formId != 0; }
    };

    [[nodiscard]] inline Selection selectStickyNearest(
        std::span<const Candidate> candidates,
        const std::uint32_t activeFormId,
        const float maximumDistanceSquared) noexcept
    {
        if (!std::isfinite(maximumDistanceSquared) || maximumDistanceSquared < 0.0f) {
            return {};
        }

        if (activeFormId != 0) {
            for (const auto& candidate : candidates) {
                if (candidate.formId == activeFormId && candidate.eligible &&
                    std::isfinite(candidate.distanceSquared) && candidate.distanceSquared >= 0.0f &&
                    candidate.distanceSquared <= maximumDistanceSquared) {
                    return { candidate.formId, candidate.distanceSquared };
                }
            }
        }

        Selection nearest{};
        for (const auto& candidate : candidates) {
            if (!candidate.eligible || candidate.formId == 0 ||
                !std::isfinite(candidate.distanceSquared) || candidate.distanceSquared < 0.0f ||
                candidate.distanceSquared > maximumDistanceSquared) {
                continue;
            }
            if (!nearest || candidate.distanceSquared < nearest.distanceSquared ||
                (candidate.distanceSquared == nearest.distanceSquared && candidate.formId < nearest.formId)) {
                nearest = { candidate.formId, candidate.distanceSquared };
            }
        }
        return nearest;
    }
}
