#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace rock_backpack::panel_math
{
    struct Vec3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    struct StoredTransform
    {
        // Row-major copy of RE::NiTransform::rotate. ROCK Prober's proven Prisma
        // convention treats rows 0/1/2 as local X/Y/Z axes in world space.
        std::array<float, 9> rotation{};
        Vec3 translation{};
        float scale{ 1.0f };
    };

    struct PanelPose
    {
        Vec3 position{};
        Vec3 right{};
        Vec3 up{};
        Vec3 front{};
        // Prisma requires X,Y,Z,W.
        std::array<float, 4> orientation{};
    };

    [[nodiscard]] inline bool isFinite(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] inline Vec3 operator+(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
    }

    [[nodiscard]] inline Vec3 operator-(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
    }

    [[nodiscard]] inline Vec3 operator*(const Vec3& value, const float scalar) noexcept
    {
        return { value.x * scalar, value.y * scalar, value.z * scalar };
    }

    [[nodiscard]] inline float dot(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    [[nodiscard]] inline Vec3 cross(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return {
            lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x,
        };
    }

    [[nodiscard]] inline std::optional<Vec3> normalize(const Vec3& value) noexcept
    {
        if (!isFinite(value)) {
            return std::nullopt;
        }
        const float lengthSquared = dot(value, value);
        if (!std::isfinite(lengthSquared) || lengthSquared < 0.000001f) {
            return std::nullopt;
        }
        return value * (1.0f / std::sqrt(lengthSquared));
    }

    [[nodiscard]] inline std::optional<std::array<float, 4>> quaternionFromBasis(
        const Vec3& right,
        const Vec3& up,
        const Vec3& front) noexcept
    {
        // Conventional column-vector matrix whose columns are Prisma local +X/+Y/+Z in world space.
        const float m00 = right.x;
        const float m01 = up.x;
        const float m02 = front.x;
        const float m10 = right.y;
        const float m11 = up.y;
        const float m12 = front.y;
        const float m20 = right.z;
        const float m21 = up.z;
        const float m22 = front.z;

        std::array<float, 4> quaternion{};
        const float trace = m00 + m11 + m22;
        if (trace > 0.0f) {
            const float scale = std::sqrt(trace + 1.0f) * 2.0f;
            if (!std::isfinite(scale) || scale < 0.0001f) {
                return std::nullopt;
            }
            quaternion[3] = 0.25f * scale;
            quaternion[0] = (m21 - m12) / scale;
            quaternion[1] = (m02 - m20) / scale;
            quaternion[2] = (m10 - m01) / scale;
        } else if (m00 > m11 && m00 > m22) {
            const float scale = std::sqrt(std::max(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
            if (!std::isfinite(scale) || scale < 0.0001f) {
                return std::nullopt;
            }
            quaternion[3] = (m21 - m12) / scale;
            quaternion[0] = 0.25f * scale;
            quaternion[1] = (m01 + m10) / scale;
            quaternion[2] = (m02 + m20) / scale;
        } else if (m11 > m22) {
            const float scale = std::sqrt(std::max(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
            if (!std::isfinite(scale) || scale < 0.0001f) {
                return std::nullopt;
            }
            quaternion[3] = (m02 - m20) / scale;
            quaternion[0] = (m01 + m10) / scale;
            quaternion[1] = 0.25f * scale;
            quaternion[2] = (m12 + m21) / scale;
        } else {
            const float scale = std::sqrt(std::max(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
            if (!std::isfinite(scale) || scale < 0.0001f) {
                return std::nullopt;
            }
            quaternion[3] = (m10 - m01) / scale;
            quaternion[0] = (m02 + m20) / scale;
            quaternion[1] = (m12 + m21) / scale;
            quaternion[2] = 0.25f * scale;
        }

        float normSquared = 0.0f;
        for (const float component : quaternion) {
            if (!std::isfinite(component)) {
                return std::nullopt;
            }
            normSquared += component * component;
        }
        if (!std::isfinite(normSquared) || normSquared < 0.000001f) {
            return std::nullopt;
        }
        const float inverseNorm = 1.0f / std::sqrt(normSquared);
        for (float& component : quaternion) {
            component *= inverseNorm;
        }
        return quaternion;
    }

    [[nodiscard]] inline Vec3 rotateLocalVectorToWorld(
        const StoredTransform& transform,
        const Vec3& localVector) noexcept
    {
        const Vec3 localX{ transform.rotation[0], transform.rotation[1], transform.rotation[2] };
        const Vec3 localY{ transform.rotation[3], transform.rotation[4], transform.rotation[5] };
        const Vec3 localZ{ transform.rotation[6], transform.rotation[7], transform.rotation[8] };
        return (localX * localVector.x) + (localY * localVector.y) + (localZ * localVector.z);
    }

    [[nodiscard]] inline Vec3 transformLocalPointToWorld(
        const StoredTransform& transform,
        const Vec3& localPoint) noexcept
    {
        return transform.translation + (rotateLocalVectorToWorld(transform, localPoint) * transform.scale);
    }

    [[nodiscard]] inline std::optional<PanelPose> faceViewer(Vec3 position, Vec3 viewer, Vec3 viewerRight)
    {
        const auto front = normalize(viewer - position);
        if (!front) return {};
        auto right = normalize(cross(Vec3{0, 0, 1}, *front));
        if (!right) right = normalize(viewerRight - *front * dot(viewerRight, *front));
        if (!right) return {};
        if (dot(*right, viewerRight) < 0) *right = *right * -1.f;
        const auto up = normalize(cross(*front, *right));
        if (!up) return {};
        const auto orientation = quaternionFromBasis(*right, *up, *front);
        if (!orientation) return {};
        return PanelPose{position, *right, *up, *front, *orientation};
    }

    // The left-shoulder backpack pull prompt: in view, low and to the left,
    // so the hand reaching over the shoulder never covers it.
    [[nodiscard]] inline std::optional<PanelPose> pullPromptPose(Vec3 eye, Vec3 forward, Vec3 right)
    {
        const auto f = normalize(forward), r = normalize(right);
        if (!f || !r) return {};
        const auto up = normalize(cross(*r, *f));
        if (!up) return {};
        return faceViewer(eye + *f * 42.f - *r * 9.f - *up * 13.f, eye, *r);
    }

    [[nodiscard]] inline std::optional<PanelPose> computeBackpackPanelPose(
        const StoredTransform& rootWorld,
        const Vec3& localOffset = {}) noexcept
    {
        if (!isFinite(rootWorld.translation) || !isFinite(localOffset) || !std::isfinite(rootWorld.scale) ||
            std::fabs(rootWorld.scale) < 0.0001f || std::fabs(rootWorld.scale) > 100.0f) {
            return std::nullopt;
        }
        for (const float value : rootWorld.rotation) {
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
        }

        // Derived directly from Go.nif's largest connected outer-facing surface after its
        // internal ground-model transform. This is the visible backpack face, not a guessed
        // engine axis: the patch ESP remains only the form registry while the NIF is pose authority.
        constexpr Vec3 localSurfaceCenter{ -0.1214f, 0.1447f, 6.7465f };
        constexpr Vec3 localFront{ -0.00005f, -0.16605f, 0.98612f };
        constexpr Vec3 localRight{ -1.0f, 0.0f, -0.00005f };
        constexpr float outwardClearance = 0.75f;

        const Vec3 rawRight = rotateLocalVectorToWorld(rootWorld, localRight);
        const Vec3 rawFront = rotateLocalVectorToWorld(rootWorld, localFront);
        const Vec3 rawUp = rotateLocalVectorToWorld(rootWorld, cross(localFront, localRight));

        const auto front = normalize(rawFront);
        if (!front) {
            return std::nullopt;
        }
        auto right = normalize(rawRight - (*front * dot(rawRight, *front)));
        if (!right) {
            return std::nullopt;
        }
        auto up = normalize(cross(*front, *right));
        if (!up || dot(*up, rawUp) < 0.0f) {
            right = normalize(*right * -1.0f);
            up = right ? normalize(cross(*front, *right)) : std::nullopt;
        }
        if (!right || !up) {
            return std::nullopt;
        }

        const auto orientation = quaternionFromBasis(*right, *up, *front);
        if (!orientation) {
            return std::nullopt;
        }

        // Position and orientation are composed from the same live object-local
        // basis. Configurator offsets therefore rotate rigidly with the backpack.
        const Vec3 surfaceCenter = transformLocalPointToWorld(rootWorld, localSurfaceCenter + localOffset);
        const Vec3 position = surfaceCenter + (*front * (outwardClearance * std::fabs(rootWorld.scale)));
        if (!isFinite(position)) {
            return std::nullopt;
        }
        return PanelPose{
            .position = position,
            .right = *right,
            .up = *up,
            .front = *front,
            .orientation = *orientation,
        };
    }
    [[nodiscard]] inline std::optional<PanelPose> anchoredInventoryPose(const StoredTransform& bag, float width, Vec3 offset,
        bool bagInRightHand = false, float yawDegrees = 0.f)
    {
        if (!std::isfinite(width) || width <= 0 || !std::isfinite(yawDegrees)) return {};
        auto pose = computeBackpackPanelPose(bag,offset);
        if (pose) {
            const float side=bagInRightHand?-1.f:1.f;
            pose->position = pose->position + pose->right * (side * (13.14f * bag.scale + width * .5f + 2.f));
            // Mirror the angle with the side, keeping text upright and facing out.
            const float angle=yawDegrees * side * .01745329252f;
            const auto right=pose->right, front=pose->front;
            pose->right=right*std::cos(angle)+front*std::sin(angle);
            pose->front=front*std::cos(angle)-right*std::sin(angle);
            const auto q=quaternionFromBasis(pose->right,pose->up,pose->front);
            if(!q) return {}; pose->orientation=*q;
        }
        return pose;
    }
}
