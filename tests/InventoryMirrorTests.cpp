#include "InventoryMirror.h"
#include <cstdlib>
#include <iostream>
using namespace rock_backpack;
using namespace inventory_mirror;
using namespace storage;
static void check(bool value,const char* why) { if(!value) { std::cerr<<why<<'\n'; std::exit(1); } }
static Stack item(std::uint32_t form,Kind kind,std::uint32_t count,std::uint64_t witness,std::uint16_t uid=0)
{ Stack s; s.form=form;s.kind=kind;s.count=count;s.stackWitness=witness;s.name=std::to_string(form);s.uniqueBase=uid?20:0;s.uniqueIndex=uid;return s; }
int main()
{
    Ledger ledger; ledger.grid=Grid(2,1);
    std::vector<Stack> native{item(100,Kind::Pistol,1,1000,1),item(200,Kind::Aid,6,2000),item(300,Kind::Misc,100,3000)};
    check(ledger.reconcile(native),"initial snapshot rejected");
    check(ledger.records.size()==3 && ledger.grid.entries().size()==3,"native inventory not mirrored exactly once");
    const auto pistol=ledger.records[0].id,aid=ledger.records[1].id,misc=ledger.records[2].id;
    check(ledger.grid.find(aid)->placement.location==Location::Overflow,"existing overflow was lost");
    check(ledger.grid.find(misc)->placement.location==Location::Interior,"Misc consumed overflow capacity");
    check(ledger.grid.move(pistol,{Location::Pistol})==Result::Ok,"mount assignment failed");
    check(ledger.reconcile(native) && ledger.grid.find(aid)->placement.location==Location::Interior,"freed space did not resolve overflow");
    check(ledger.grid.find(pistol)->placement.location==Location::Pistol,"refresh changed an exact pinned weapon");
    native[0].stackWitness=9999;
    check(ledger.reconcile(native) && ledger.find(pistol)->observed.stackWitness==9999,"native UID did not survive stack replacement");
    native[0].equipped=true;
    check(ledger.reconcile(native) && ledger.grid.find(pistol)->placement.location==Location::Equipped,"equipping did not free holster while preserving existing item");
    native.erase(native.begin());
    check(ledger.reconcile(native) && !ledger.find(pistol),"externally removed item remained drawable");
    check(ledger.grid.find(aid)->count==6 && ledger.grid.find(misc)->count==100,"removal changed unrelated quantities");

    Ledger split; split.grid=Grid(2,1);
    std::vector<Stack> ammo{item(400,Kind::Ammo,10,4000)};
    check(split.reconcile(ammo),"ammo setup");
    const auto original=split.records[0].id,newId=split.nextId++;
    auto second=split.records[0]; second.id=newId;
    check(split.grid.split(original,newId,4,{Location::Interior,1,0})==Result::Ok,"logical stack split failed");
    split.records.push_back(second);
    ammo[0].count=9;
    check(split.reconcile(ammo,newId),"native consumption after logical split failed");
    check(split.grid.find(original)->count==6 && split.grid.find(newId)->count==3,"native consumption debited wrong logical pile");
    ammo[0].count=12;
    check(split.reconcile(ammo),"native compatible stack growth failed");
    check(split.grid.find(original)->count+split.grid.find(newId)->count==12,"native stack growth duplicated items");

    // A legacy logical pile may remain overflow while another pile sharing
    // its native stack has a grid cell. Allowed growth must use that cell,
    // never increase the grandfathered overflow quantity.
    check(split.grid.move(newId,{Location::Overflow})==Result::Ok,"overflow split setup");
    auto blocker=item(401,Kind::Aid,1,4100);
    ammo.push_back(blocker);
    check(split.reconcile(ammo),"blocker setup");
    // Keep the second pile explicitly overflow, occupying its cell with aid.
    check(split.grid.move(newId,{Location::Overflow})==Result::Ok,"overflow pile relocation");
    const auto blockerId=split.records.back().id;
    check(split.grid.move(blockerId,{Location::Interior,1,0})==Result::Ok,"blocker placement");
    const auto beforeGrowth=split.grid.find(newId)->count;
    ammo[0].count+=3;
    check(split.reconcile(ammo) && split.grid.find(newId)->count==beforeGrowth,"compatible growth increased grandfathered overflow");
    check(split.grid.find(original)->count+split.grid.find(newId)->count==15,"growth changed native total");
    const auto stablePlacement=split.grid.find(original)->placement;
    check(split.reconcile(ammo,original) && split.grid.find(original)->placement==stablePlacement,
        "an action without native consumption unnecessarily rearranged its selected pile");

    Ledger ambiguous; ambiguous.grid=Grid(4,1);
    std::vector<Stack> old{item(500,Kind::Pistol,1,5000)};
    check(ambiguous.reconcile(old),"ambiguity setup");
    const auto previous=ambiguous.records[0].id;
    ambiguous.records[0].observed.stackWitness=0; // Simulate a saved presentation, never a saved engine pointer.
    std::vector<Stack> variants{item(500,Kind::Pistol,1,6000),item(500,Kind::Pistol,1,7000)};
    check(ambiguous.reconcile(variants) && !ambiguous.find(previous) && ambiguous.records.size()==2,
        "ambiguous unpinned variants inherited a guessed identity");
    variants.clear();
    check(ambiguous.reconcile(variants) && ambiguous.records.empty() && ambiguous.grid.entries().empty(),"empty native inventory left phantom records");

    Ledger held; held.grid=Grid(2,1);
    auto gun=item(601,Kind::Pistol,3,6100); gun.equipped=true;
    std::vector<Stack> stock{gun,item(602,Kind::Aid,1,6200)};
    check(held.reconcile(stock),"equipped stack import");
    auto countAt=[&](Location location) { unsigned count{}; for(const auto& e:held.grid.entries()) if(e.kind==Kind::Pistol && e.placement.location==location) count+=e.count; return count; };
    check(countAt(Location::Equipped)==1 && countAt(Location::Interior)==2,"whole equipped stack incorrectly exempted");
    check(held.reconcile(stock) && countAt(Location::Equipped)==1 && countAt(Location::Interior)==2,"equipped refresh duplicated or lost units");
    stock[0].count=1;
    check(held.reconcile(stock) && countAt(Location::Equipped)==1 && countAt(Location::Interior)==0,"equipped singleton uses interior");
    check(held.grid.firstFit(Kind::Aid).has_value(),"equipped singleton did not free cells");
    stock.push_back(item(603,Kind::Aid,1,6300));
    check(held.reconcile(stock) && !held.grid.firstFit(Kind::Pistol),"full bag unexpectedly admits equipped stow");
    check(countAt(Location::Equipped)==1 && countAt(Location::Overflow)==0,"full interior counted equipped gun as overflow");
    stock[0].equipped=false; // An external/native unequip must preserve, never delete, a gun.
    check(held.reconcile(stock) && countAt(Location::Equipped)==0 && countAt(Location::Overflow)==1,"external unequip lost grandfathered gun");
    stock.erase(stock.begin()+1,stock.end());
    check(held.reconcile(stock) && countAt(Location::Interior)==1,"unequipped gun did not reclaim available interior");
    stock[0].equipped=true;
    check(held.reconcile(stock) && held.grid.spatialIndices().empty(),"in-hand weapon remained a grid hit target");
    for(auto& record:held.records) record.observed.stackWitness=0;
    check(held.reconcile(stock) && countAt(Location::Equipped)==1,"saved equipped layout failed to reconcile");

    Ledger pending; pending.grid=Grid(3,1);
    auto reservedWeapon=item(701,Kind::LongGun,1,7100); reservedWeapon.equipped=true;
    std::vector<Stack> pendingItems{reservedWeapon};
    check(pending.reconcile(pendingItems),"pending stow setup");
    check(pending.grid.reserveInterior(Kind::LongGun,{Location::Interior}),"stow reservation failed");
    check(!pending.grid.firstFit(Kind::Aid),"pickup consumed pending stow reservation");
    pendingItems.push_back(item(702,Kind::Aid,1,7200)); // forced reward remains safe overflow
    check(pending.reconcile(pendingItems) && pending.grid.reservation().has_value(),"refresh discarded pending reservation");
    check(pending.grid.entries().back().placement.location==Location::Overflow,"forced reward consumed reserved cells");
    pending.grid.clearReservation(); pendingItems[0].equipped=false;
    check(pending.reconcile(pendingItems),"confirmed stow reconcile");
    check(pending.grid.entries().front().placement.location==Location::Interior,"reserved weapon could not stow");
}
