#include "InventoryCapacity.h"
#include "ui/PanelModel.h"
#include <cstdlib>
#include <iostream>

using namespace rock_backpack;
static void check(bool value,const char* reason)
{ if(!value) { std::cerr<<reason<<'\n'; std::exit(1); } }
int main()
{
    using namespace storage;
    using namespace inventory_capacity;
    Grid grid(2,1);
    check(grid.insert({1,Kind::Pistol})==Result::Ok,"setup");
    check(!evaluate(grid,Kind::Aid,false,Source::Voluntary).accepted,"voluntary overflow accepted");
    for(auto source:{Source::ExistingInventory,Source::ScriptedGrant}) {
        const auto result=evaluate(grid,Kind::Armor,false,source);
        check(result.accepted && result.placement && result.placement->location==Location::Overflow,"existing/forced content lost");
    }
    const auto misc=evaluate(grid,Kind::Misc,false,Source::Voluntary);
    check(misc.accepted && misc.placement->location==Location::Interior,"full backpack blocked Misc");
    check(!evaluate(grid,Kind::Junk,false,Source::Voluntary).accepted,"Junk was incorrectly treated as Misc");
    check(evaluate(grid,Kind::Aid,true,Source::Voluntary).merge,"compatible stack growth required a new cell");
    check(!evaluate(grid,Kind(255),false,Source::ScriptedGrant).accepted,"invalid kind admitted");
    check(grid.insert({2,Kind::Armor,1,0,{Location::Overflow}})==Result::Ok,"overflow record rejected");
    check(grid.spatialIndices().size()==1 && grid.at(Location::Interior,0,0)->id==1,"overflow polluted physical hit tests");
    check(grid.move(2,{Location::Interior})==Result::Occupied,"overflow entry overwrote another item");
    check(grid.erase(1) && grid.move(2,{Location::Interior})==Result::Ok,"overflow could not occupy newly freed space");

    Grid batch(2,1); unsigned accepted{};
    for(unsigned i=1;i<=4;++i) {
        const auto admission=evaluate(batch,Kind::Aid,false,Source::Voluntary);
        if(admission.accepted) { check(batch.insert({i,Kind::Aid,1,0,*admission.placement})==Result::Ok,"batch reservation failed"); ++accepted; }
    }
    check(accepted==2 && batch.entries().size()==2,"Take All did not reserve each accepted footprint");
    ui::Model model; model.grid=Grid(1,1);
    check(model.grid.insert({5,Kind::Heavy,1,0,{Location::Overflow}})==Result::Ok,"model overflow setup");
    check(model.grid.insert({6,Kind::Misc})==Result::Ok,"model misc setup");
    ui::Item weapon; weapon.storageId=5; weapon.category=inventory_policy::Category::Weapons;
    ui::Item key; key.storageId=6; key.category=inventory_policy::Category::Misc;
    model.items={weapon,key}; model.indexItems();
    check(model.overflow.size()==1 && model.items[model.overflow[0]].storageId==5,"overflow not accessible in UI");
    check(model.miscellaneous.size()==1 && model.items[model.miscellaneous[0]].storageId==6,"Misc mixed with capacity overflow");

    for(auto kind:{Kind::LongGun,Kind::Pistol,Kind::Sniper,Kind::Heavy,Kind::Melee,Kind::Knife}) {
        Grid full(2,1); check(full.insert({1,Kind::Pistol})==Result::Ok,"full interior setup");
        check(!forDestination(full,kind,Location::Interior).accepted,"full bag accepted interior stow");
        for(auto slot:{Location::LeftRifle,Location::RightRifle,Location::Pistol}) {
            const auto admission=forDestination(full,kind,slot);
            check(admission.accepted && admission.placement && admission.placement->location==slot,"full interior blocked external loose/equipped stow");
            check(full.insert({10+unsigned(slot),kind,1,0,*admission.placement})==Result::Ok,"external insertion used interior space");
            check(!forDestination(full,kind,slot).accepted,"occupied mount accepted another weapon");
        }
        check(full.find(1)->placement.location==Location::Interior,"external stow displaced interior item");
    }
}
