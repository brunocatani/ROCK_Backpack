#include "StorageGrid.h"
#include "StorageGesture.h"
#include "BackpackZones.h"
#include <cstdlib>
#include <iostream>
using namespace rock_backpack::storage;
void check(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
int main()
{
    DeliveryGesture delivery;
    delivery.delivered(11);
    check(!delivery.permits(11,true,true,false), "draw press cannot arm an immediate restash");
    check(!delivery.permits(11,true,false,false), "draw release keeps the delivered item in hand");
    check(!delivery.permits(11,false,false,true), "tracking loss cannot invent a fresh press");
    check(delivery.permits(11,true,false,false), "a completed draw release kept storage gated indefinitely");
    check(delivery.permits(11,true,true,true), "fresh press arms the next release for storage");
    check(delivery.permits(11,true,false,false), "intentional release can store directly into a mount");
    namespace zones = rock_backpack::zones;
    namespace math = rock_backpack::panel_math;
    math::StoredTransform root{{0,1,0,-1,0,0,0,0,1}, {20,30,40}, 2};
    for (const auto& region : zones::depositRegions) {
        // Native compose RVA 1A8D60 weights stored row axes: this matrix
        // turns local +X to world +Y. Independent of the UI helper under test.
        const math::Vec3 world{20-2*region.center.y,30+2*region.center.x,40+2*region.center.z};
        const auto local = zones::localPoint(root, world);
        check(local && zones::locate(*local, true) == region.location, "Zones follow translated, rotated, scaled backpack");
        const auto guide=zones::guidancePose(root,region.location);
        check(guide.has_value(), "guide follows a valid backpack transform");
        const auto guided=guide->position;
        const auto guideLocal=zones::localPoint(root,guided);
        check(guideLocal && zones::locate(*guideLocal,true)==region.location, "rendered marker center is inside the actual deposit zone");
    }
    check(zones::locate({15,-15,8}) == Location::LeftRifle, "Full weapon length remains available for draw");
    check(!zones::locate({15,-15,8},true), "Rifle tip is not a deposit region");
    check(zones::locate(zones::mount(Location::LeftRifle),true)==Location::LeftRifle, "Rifle marker and mount share their deposit center");
    check(zones::locate(zones::mount(Location::Pistol),true)==Location::Pistol, "Pistol marker and mount share their deposit center");
    check(!zones::locate({0,0,50}), "Outside backpack rejects deposit");
    const auto rifleMask=zones::depositMask(Kind::LongGun);
    check(rifleMask & (1u<<unsigned(Location::Pistol)), "Bottom mount must accept a rifle");
    check(zones::choose({15,-2,8},true,rifleMask)==Location::LeftRifle, "Hand inside rifle marker selects the mount");
    check(zones::choose({0,-2,17},true,rifleMask)==Location::Interior, "Interior marker remains separate from side mounts");
    check(!zones::choose({40,0,8},true,rifleMask), "Weapon geometry cannot deposit with the hand outside the guides");
    check(zones::choose({0,17,8},true,rifleMask)==Location::Pistol, "Rifle cannot target bottom weapon mount");
    check(zones::choose({15,2.5f,8},true,rifleMask,Location::LeftRifle)==Location::LeftRifle, "Small edge movement retains selected guide");
    check(!zones::choose({15,2.5f,8},true,rifleMask), "Exit tolerance does not enlarge initial entry");
    check(!zones::choose({15,3.1f,8},true,rifleMask,Location::LeftRifle), "Leaving tolerance clears selection");
    Grid mounts;
    check(zones::drawMask(mounts)==0,"Empty mounts cannot be selected");
    mounts.insert({80,Kind::LongGun,1,0,{Location::RightRifle}});
    check(zones::choose({-15,-2,8},false,zones::drawMask(mounts))==Location::RightRifle,"Only populated mounts advertise a draw");
    mounts.setBusy(80,true);
    check(zones::drawMask(mounts)==0,"In-flight draw no longer advertises selection");
    const auto panel = math::anchoredInventoryPose(root,40,{});
    const auto button = math::computeBackpackPanelPose(root,{});
    check(panel && button && math::dot(panel->position-button->position,button->right) > 40, "Inventory stays on the bag's right side");
    check(panel->front.x == button->front.x && panel->front.y == button->front.y && panel->front.z == button->front.z, "Panel orientation is rigidly bag anchored");
    Grid bag(4, 3);
    check(bag.insert({1, Kind::LongGun, 1, 0, {Location::Interior, 1, 1}}) == Result::Ok, "rifle admission");
    check(bag.at(Location::Interior, 3, 1)->id == 1, "all occupied cells resolve to the same instance");
    check(bag.insert({2, Kind::Aid, 20, 7, {Location::Interior, 2, 1}}) == Result::Occupied, "overlap refused");
    check(bag.move(1, {Location::Interior, 0, 1, true}) == Result::Outside, "rotated boundary refused");
    check(bag.find(1)->placement.x == 1, "failed move preserves placement");
    check(bag.move(1, {Location::Interior, 0, 0, true}) == Result::Ok, "rotated rifle fits");
    check(bag.insert({2, Kind::Aid, 20, 7, {Location::Interior, 1, 0}}) == Result::Ok, "aid stack consumes one cell");
    check(bag.move(2, {Location::LeftRifle}) == Result::InvalidQuantity, "stack cannot occupy mount");
    check(bag.split(2, 3, 8, {Location::Interior, 2, 0}) == Result::Ok, "split into free cell");
    check(bag.find(2)->count == 12 && bag.find(3)->count == 8, "split quantity conserved");
    bag.setBusy(3, true);
    check(bag.merge(3, 2) == Result::Busy && bag.move(3, {}) == Result::Busy, "in-flight item immutable");
    bag.setBusy(3, false);
    check(bag.merge(3, 2) == Result::Ok && bag.find(2)->count == 20 && !bag.find(3), "merge quantity conserved");
    check(bag.insert({4, Kind::Aid, 1, 8, {Location::Interior, 2, 0}}) == Result::Ok, "different native stack");
    check(bag.merge(4, 2) == Result::Incompatible, "same kind does not imply instance compatibility");
    check(bag.move(1, {Location::LeftRifle}) == Result::Ok, "rifle mount");
    check(bag.at(Location::Interior, 0, 2) == nullptr, "mount does not occupy interior cells");
    for(auto kind:{Kind::Pistol,Kind::Knife,Kind::Melee,Kind::Heavy,Kind::Sniper,Kind::LongGun}) {
        Grid shoulders;
        check(shoulders.insert({55,kind,1,0,{Location::RightRifle}})==Result::Ok && shoulders.at(Location::RightRifle)->id==55,
            "right shoulder must store the exact weapon in the existing right backpack slot");
        check(shoulders.insert({56,kind,1,0,{Location::LeftRifle}})==Result::Ok && shoulders.at(Location::LeftRifle)->id==56,
            "left shoulder must own an independent item in the existing left backpack slot");
        check(shoulders.erase(56) && shoulders.at(Location::RightRifle)->id==55,"left shoulder draw must leave the right item untouched");
    }
    check(!accepts(Location::RightRifle,Kind::Aid) && !accepts(Location::RightRifle,Kind::Throwable),"shoulder rejects non-holsterable items");
    check(bag.insert({5, Kind::Pistol, 1, 0, {Location::Pistol}}) == Result::Ok, "pistol mount");
    check(bag.move(5, {Location::Pistol, 1}) == Result::Outside, "noncanonical mount placement rejected");
    check(bag.insert({6, Kind(255), 1, 0, {}}) == Result::Incompatible, "corrupt saved kind refused");
    check(bag.insert({6, Kind::Aid, 1, 0, {Location(255)}}) == Result::Incompatible, "corrupt saved location refused");
    Grid small(1, 1);
    check(!small.firstFit(Kind::Armor), "two-cell object cannot fit single cell");
    check(small.insert({1, Kind::Aid}) == Result::Ok && !small.firstFit(Kind::Aid), "full bag refused");
    Grid invalid(0, 0); check(!invalid.firstFit(Kind::Aid), "invalid dimensions fail closed");
    check(footprint(Kind::Pistol).width==2 && footprint(Kind::LongGun).width==3 && footprint(Kind::Sniper).width==4,
        "pistols, rifles and snipers must occupy 2, 3 and 4 cells");
    check(footprint(Kind::Melee).width==3 && footprint(Kind::Knife).width==2,"knives are smaller than other melee weapons");
    check(weaponKind(true,true,false,false,false,false)==Kind::Knife &&
        weaponKind(true,false,false,false,false,false)==Kind::Melee &&
        weaponKind(false,false,false,true,false,false)==Kind::Sniper &&
        weaponKind(false,false,true,true,true,false)==Kind::Heavy &&
        weaponKind(false,false,false,false,false,true)==Kind::Throwable,"weapon family precedence is inconsistent");
    Grid heavy(3,2);
    check(heavy.insert({1,Kind::Heavy})==Result::Ok && !heavy.firstFit(Kind::Aid),"heavy weapon must fill a 3x2 board");
    for(unsigned y=0;y<2;++y) for(unsigned x=0;x<3;++x)
        check(heavy.at(Location::Interior,x,y)->id==1,"heavy footprint has a gap");
    Grid tall(2,3);
    check(tall.firstFit(Kind::Heavy)==Placement{Location::Interior,0,0,true},"heavy weapon must rotate into 2x3");
    check(tall.insert({2,Kind::Heavy,1,0,*tall.firstFit(Kind::Heavy)})==Result::Ok && !tall.firstFit(Kind::Aid),"rotated heavy footprint must fill all six cells");
    check(!Grid(3,1).firstFit(Kind::Sniper) && Grid(4,1).firstFit(Kind::Sniper).has_value(),"sniper requires four consecutive cells");
    for(ItemId id=10;id<610;++id) check(heavy.insert({id,Kind::Misc,2,id})==Result::Ok,"misc list must remain open beyond the old entry cap with a full grid");
    check(heavy.spatialIndices().size()==1 && heavy.at(Location::Interior,0,0)->id==1,"misc entries must never affect grid hit tests");
    check(heavy.split(10,700,1,{})==Result::Ok && heavy.merge(700,10)==Result::Ok,"misc stacks must split and merge without free cells");
    check(heavy.move(10,{Location::LeftRifle})==Result::InvalidQuantity && heavy.move(11,{Location::Interior,1,1})==Result::Outside,"misc items cannot be placed on mounts or grid cells");
    check(heavy.erase(10) && heavy.erase(1) && heavy.firstFit(Kind::Heavy).has_value(),"list erasure must not corrupt the spatial index");
}
