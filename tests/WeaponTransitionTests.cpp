#include "WeaponTransition.h"
#include <cstdlib>
#include <iostream>

using namespace rock_backpack;
using panel_math::StoredTransform;
using panel_math::Vec3;

static void require(bool value, const char* why) { if (!value) { std::cerr << why << '\n'; std::exit(1); } }
static bool near(float a, float b) { return std::abs(a - b) < .0005f; }
static bool same(const StoredTransform& a, const StoredTransform& b)
{
    for (unsigned i = 0; i < 9; ++i) if (!near(a.rotation[i], b.rotation[i])) return false;
    return near(a.translation.x, b.translation.x) && near(a.translation.y, b.translation.y) &&
        near(a.translation.z, b.translation.z) && near(a.scale, b.scale);
}
// Engine composition with stored rows: world rotation = local * parent, and
// the position goes through the parent's row axes and scale.
static StoredTransform compose(const StoredTransform& parent, const StoredTransform& local)
{
    StoredTransform world;
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
        for (unsigned k = 0; k < 3; ++k) world.rotation[r * 3 + c] += local.rotation[r * 3 + k] * parent.rotation[k * 3 + c];
    world.translation = panel_math::transformLocalPointToWorld(parent, local.translation);
    world.scale = local.scale * parent.scale;
    return world;
}
static StoredTransform turn(float x, float y, float z, Vec3 position, float scale)
{
    StoredTransform result{{1, 0, 0, 0, 1, 0, 0, 0, 1}, position, scale};
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
    const std::array<float, 9> rx{1, 0, 0, 0, cx, sx, 0, -sx, cx}, ry{cy, 0, -sy, 0, 1, 0, sy, 0, cy}, rz{cz, sz, 0, -sz, cz, 0, 0, 0, 1};
    for (const auto& m : {rx, ry, rz}) {
        std::array<float, 9> next{};
        for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) for (unsigned k = 0; k < 3; ++k)
            next[r * 3 + c] += result.rotation[r * 3 + k] * m[k * 3 + c];
        result.rotation = next;
    }
    return result;
}

int main()
{
    const auto parent = turn(.4f, -1.1f, 2.3f, {120, -35, 9}, 1.3f);
    const auto local = turn(-.7f, .2f, 1.9f, {4, -11, 6}, .8f);
    const auto recovered = weapon_transition::localPose(parent, compose(parent, local));
    require(recovered && same(*recovered, local), "localPose is not the inverse of the engine's stored-row composition");

    const auto other = turn(1.2f, .9f, -.3f, {-7, 2, 15}, 1.f);
    require(same(weapon_transition::blend(local, other, 0), local) && same(weapon_transition::blend(local, other, 1), other),
        "blend does not start and end on its poses");
    const auto middle = weapon_transition::blend(local, other, .5f);
    for (unsigned row = 0; row < 3; ++row) {
        const Vec3 axis{middle.rotation[row * 3], middle.rotation[row * 3 + 1], middle.rotation[row * 3 + 2]};
        require(near(panel_math::dot(axis, axis), 1), "blended rotation is not orthonormal");
    }
    weapon_transition::Ease ease;
    ease.start(local, .2f);
    require(same(ease.advance(other, 0), local), "ease moved without time");
    (void)ease.advance(other, .1f);
    require(ease.active && same(ease.advance(other, .2f), other) && !ease.active, "ease does not end on its target");
    return 0;
}
