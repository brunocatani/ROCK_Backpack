#pragma once
#include "PanelPoseMath.h"
#include "StorageGrid.h"

namespace rock_backpack::zones
{
    using panel_math::Vec3;
    // Military Backpack 2 world mesh: X width, -Y top, +Z opening. Bounds
    // [-12.955,-18.906,-5.375]..[13.134,15.094,18.152], verified by nif_mcp.
    // The saved hand poses and the model itself are not changed by these zones.
    struct Region { storage::Location location; Vec3 center, half; };
    // Editable placement belongs only to stored weapon visuals. All interaction
    // regions and guides stay fixed on the backpack independently of these offsets.
    struct Layout {
        std::array<Vec3,3> positions{{{15,-2,8},{-15,-2,8},{0,17,8}}};
        std::array<Vec3,3> rotations{}; // X/Y/Z degrees, around each weapon's original pivot
    };
    inline constexpr Vec3 mount(storage::Location location, const Layout& layout = {})
    {
        if (location != storage::Location::Interior && unsigned(location) <= 3) return layout.positions[unsigned(location)-1];
        return {0,-2,17};
    }
    inline std::array<float,9> visualRotation(storage::Location location,const Layout& layout = {},storage::Kind kind=storage::Kind::LongGun)
    {
        const unsigned slot=unsigned(location)-1;
        if(slot>=3) return {1,0,0,0,1,0,0,0,1};
        auto rotation=slot==2?std::array<float,9>{0,1,0,-1,0,0,0,0,1}:std::array<float,9>{1,0,0,0,-1,0,0,0,-1};
        const auto angles=layout.rotations[slot]*.01745329252f;
        const float x=angles.x,y=angles.y,z=angles.z;
        const std::array<std::array<float,9>,3> turns{{
            {1,0,0,0,std::cos(x),std::sin(x),0,-std::sin(x),std::cos(x)},
            {std::cos(y),0,-std::sin(y),0,1,0,std::sin(y),0,std::cos(y)},
            {std::cos(z),std::sin(z),0,-std::sin(z),std::cos(z),0,0,0,1}}};
        for(const auto& turn:turns) {
            std::array<float,9> next{};
            for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) for(unsigned k=0;k<3;++k)
                next[r*3+c]+=rotation[r*3+k]*turn[k*3+c];
            rotation=next;
        }
        // Rifles roll around their own barrel (+Y), after composing their
        // placement. This reverses the facing side without moving the pivot
        // or changing the muzzle direction, including calibrated mounts.
        const bool rifle=kind==storage::Kind::LongGun || kind==storage::Kind::Sniper || kind==storage::Kind::Heavy;
        if(slot!=2 && rifle) for(unsigned column=0;column<3;++column) {
            rotation[column]=-rotation[column];
            rotation[6+column]=-rotation[6+column];
        }
        return rotation;
    }
    inline constexpr std::array grabRegions{
        Region{storage::Location::LeftRifle, mount(storage::Location::LeftRifle), {5, 18, 12}},
        Region{storage::Location::RightRifle, mount(storage::Location::RightRifle), {5, 18, 12}},
        Region{storage::Location::Pistol, mount(storage::Location::Pistol), {13, 4, 12}},
        Region{storage::Location::Interior, {0, -2, 17}, {8.7f, 12, 11}}
    };
    // Stowing targets the middle of each mount, not the full length used to
    // find a mounted weapon. The internal cavity remains a separate inventory.
    inline constexpr std::array depositRegions{
        Region{storage::Location::LeftRifle, mount(storage::Location::LeftRifle), {5, 4, 7}},
        Region{storage::Location::RightRifle, mount(storage::Location::RightRifle), {5, 4, 7}},
        Region{storage::Location::Pistol, mount(storage::Location::Pistol), {6, 4, 7}},
        Region{storage::Location::Interior, mount(storage::Location::Interior), {8.7f, 7, 8}}
    };
    struct StashLayout {
        // Indexed by Location: interior, left rifle, right rifle, pistol.
        std::array<Vec3,4> centers{{{0,-2,17},{15,-2,8},{-15,-2,8},{0,17,8}}};
        std::array<Vec3,4> halves{{{8.7f,7,8},{5,4,7},{5,4,7},{6,4,7}}};
    };
    inline auto regions(bool depositing, const StashLayout& stash = {})
    {
        auto result=depositing?depositRegions:grabRegions;
        if(depositing) for(auto& region:result) {
            region.center=stash.centers[unsigned(region.location)];
            region.half=stash.halves[unsigned(region.location)];
        }
        return result;
    }
    // Fixed authored markers: stash calibration must not resize or move them.
    inline Region depositRegion(storage::Location location)
    {
        for(auto region:depositRegions) if(region.location==location) return region;
        return depositRegions.back();
    }
    inline std::optional<Vec3> localPoint(const panel_math::StoredTransform& root, Vec3 point)
    {
        if (!panel_math::isFinite(point) || !panel_math::isFinite(root.translation) || !std::isfinite(root.scale) || root.scale <= .0001f) return {};
        for (const auto element : root.rotation) if (!std::isfinite(element)) return {};
        const auto delta = (point - root.translation) * (1.f / root.scale);
        // FO4VR compose RVA 1A8D60 weights the stored row axes by local XYZ.
        // Dot against those same axes for the inverse; do not infer this from
        // CommonLib's conventional matrix-vector operator (HandFrame.h agrees).
        return Vec3{panel_math::dot(delta, {root.rotation[0],root.rotation[1],root.rotation[2]}),
            panel_math::dot(delta, {root.rotation[3],root.rotation[4],root.rotation[5]}),
            panel_math::dot(delta, {root.rotation[6],root.rotation[7],root.rotation[8]})};
    }
    inline bool contains(const Region& region, Vec3 point, float margin = 0)
    {
        if (!panel_math::isFinite(point)) return false;
        const auto d = point - region.center;
        return std::abs(d.x) <= region.half.x + margin && std::abs(d.y) <= region.half.y + margin && std::abs(d.z) <= region.half.z + margin;
    }
    inline unsigned depositMask(storage::Kind kind)
    {
        unsigned mask{};
        for (const auto& region : depositRegions) if (storage::accepts(region.location, kind)) mask |= 1u << unsigned(region.location);
        return mask;
    }
    inline unsigned drawMask(const storage::Grid& grid)
    {
        unsigned mask{};
        for (unsigned slot=1;slot<=3;++slot)
            if (const auto* entry=grid.at(storage::Location(slot)); entry && !entry->busy) mask |= 1u << slot;
        return mask;
    }
    inline std::optional<storage::Location> locate(Vec3 local, bool depositing = false, unsigned allowed = 15, const StashLayout& stash = {})
    {
        std::optional<storage::Location> best;
        float nearest = 4;
        for (const auto& region : regions(depositing,stash)) {
            if (!(allowed & (1u << unsigned(region.location))) || !contains(region, local)) continue;
            const auto d = local - region.center;
            const float distance = (d.x*d.x)/(region.half.x*region.half.x) + (d.y*d.y)/(region.half.y*region.half.y) + (d.z*d.z)/(region.half.z*region.half.z);
            if (distance < nearest) { best = region.location; nearest = distance; }
        }
        return best;
    }
    inline constexpr float kExitMargin = 1.f;
    struct Guidance { unsigned visible{15}, ready{}, blocked{}; };
    // Marker pose is independent of weapon placement and detection calibration.
    inline std::optional<panel_math::PanelPose> guidancePose(const panel_math::StoredTransform& root,
        storage::Location location)
    {
        if (!localPoint(root,root.translation)) return {};
        const auto right=panel_math::normalize(panel_math::rotateLocalVectorToWorld(root,{-1,0,0}));
        const auto up=panel_math::normalize(panel_math::rotateLocalVectorToWorld(root,{0,-1,0}));
        const auto front=panel_math::normalize(panel_math::rotateLocalVectorToWorld(root,{0,0,1}));
        if (!right || !up || !front) return {};
        const auto center=depositRegion(location).center;
        return panel_math::PanelPose{panel_math::transformLocalPointToWorld(root,center),*right,*up,*front,{}};
    }
    inline std::optional<storage::Location> choose(Vec3 hand, bool depositing, unsigned allowed,
        std::optional<storage::Location> previous = {}, const StashLayout& stash = {})
    {
        // The caller supplies the Weapon/WeaponLeft grip point in backpack
        // space. Do not substitute the wrist or require the wrist in this zone.
        if (auto direct = locate(hand, depositing, allowed,stash)) return direct;
        if (previous && (allowed & (1u << unsigned(*previous))))
            for (const auto& region : regions(depositing,stash))
                if (region.location == *previous && contains(region,hand,kExitMargin)) return previous;
        return std::nullopt;
    }
}
