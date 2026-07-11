#include "ui/PreviewInventory.h"
#include "../preview/Simulation.h"
#include <fstream>
#include <iostream>
#include <cstdlib>

namespace ui = rock_backpack::ui;
namespace storage = rock_backpack::storage;
void require(bool condition, const char* message)
{ if (!condition) { std::cerr << message << '\n'; std::exit(1); } }
int main(int argc, char** argv)
{
    require(argc == 2, "absolute fixture directory required");
    const auto path = std::filesystem::absolute(argv[1]) / "inventory-roundtrip.json";
    ui::Model model; model.available = true;
    model.grid.insert({1, storage::Kind::LongGun, 1, 0, {storage::Location::LeftRifle}});
    model.grid.insert({2, storage::Kind::Aid, 37, 444, {storage::Location::Interior, 7, 3}});
    model.grid.insert({3, storage::Kind::Armor, 1, 12, {storage::Location::Interior, 9, 1, true}});
    model.items = {
        {.id="1", .name="Modified rifle", .count=1, .weight=8.5f, .formId=11, .storageId=1, .appearance=1},
        {.id="2", .name="Stimpak", .category=rock_backpack::inventory_policy::Category::Aid, .count=37, .weight=.1f, .usable=true, .formId=12, .storageId=2, .appearance=2},
        {.id="3", .name="Armor", .count=1, .weight=3.5f, .formId=13, .storageId=3, .appearance=3}};
    require(ui::savePreviewInventory(model, path), "snapshot write failed");
    const auto loaded = ui::loadPreviewInventory(path);
    require(loaded && loaded->items.size() == 3, "snapshot load failed");
    require(loaded->grid.find(2)->count == 37 && loaded->grid.find(2)->stackKey == 444, "stack identity or quantity lost");
    require(loaded->grid.find(3)->placement == model.grid.find(3)->placement, "rotated footprint lost");
    require(loaded->grid.at(storage::Location::LeftRifle)->id == 1 && loaded->items[0].appearance == 1, "exterior instance/appearance lost");
    require(!ui::savePreviewInventory(model, "relative.json"), "relative output path must be refused");
    require(model.grid.insert({4,storage::Kind::Heavy,1,0,{storage::Location::Overflow}})==storage::Result::Ok,"overflow export setup");
    model.items.push_back({.id="4",.name="Existing heavy weapon",.count=1,.equipped=true,.formId=14,.storageId=4,.appearance=4});
    require(ui::savePreviewInventory(model,path),"unified snapshot write failed");
    const auto unified=ui::loadPreviewInventory(path);
    require(unified && unified->overflow.size()==1 && unified->items.back().equipped,"overflow/equipped state lost in preview snapshot");
    require(model.grid.insert({5,storage::Kind::Pistol,1,0,{storage::Location::Equipped}})==storage::Result::Ok,"equipped layout setup");
    model.items.push_back({.id="5",.name="Equipped pistol",.count=1,.equipped=true,.formId=15,.storageId=5,.appearance=5});
    require(ui::savePreviewInventory(model,path),"equipped snapshot write");
    const auto equipped=ui::loadPreviewInventory(path);
    require(equipped && equipped->heldWeapons.size()==1 && equipped->grid.find(5)->placement.location==storage::Location::Equipped,"equipped nonspatial list lost");
    { std::ofstream file(path, std::ios::trunc); file << "{\"schema\":1,\"columns\":0,\"rows\":8,\"items\":[]}"; }
    require(!ui::loadPreviewInventory(path), "invalid dimensions must fail closed");
    { std::ofstream file(path, std::ios::trunc); file << "{broken"; }
    require(!ui::loadPreviewInventory(path), "truncated snapshot must fail closed");
    ui::Model empty; empty.available=true; empty.generation=1;
    preview::Simulation sim(empty);
    sim.takeSample(storage::Kind::LongGun);
    sim.input(storage::Location::LeftRifle,true,true,false);
    sim.input(storage::Location::LeftRifle,false,false,true);
    require(!sim.open && sim.model.grid.at(storage::Location::LeftRifle), "Direct hand-to-mount deposit needs no inventory panel");
    sim.takeSample(storage::Kind::LongGun);
    sim.input(storage::Location::LeftRifle,true,true,false);
    sim.input(storage::Location::LeftRifle,false,false,true);
    require(sim.inventoryFullSeconds>0 && sim.hands[0].has_value(),"full mount must show UI notice and retain the item in hand");
    sim.hands[0].reset();
    sim.input(storage::Location::LeftRifle,true,true,false);
    require(sim.hands[0].has_value(), "Empty hand can draw directly from the physical mount");
    sim.input(storage::Location::LeftRifle,false,false,true);
    require(sim.hands[0].has_value() && !sim.model.grid.at(storage::Location::LeftRifle), "Initial draw release must retain the weapon");
    require(!sim.canStash(storage::Location::LeftRifle), "Initial release cannot advertise stash readiness");
    sim.input(storage::Location::LeftRifle,true,true,false);
    require(sim.canStash(storage::Location::LeftRifle), "Fresh press arms valid green stash cue");
    sim.input(storage::Location::LeftRifle,false,false,true);
    sim.takeSample(storage::Kind::Aid);
    require(!sim.canStash(storage::Location::RightRifle), "Incompatible mount never advertises green feedback");
    sim.input(storage::Location::Interior,true,true,false);
    sim.input(storage::Location::Interior,false,false,true);
    require(sim.model.items.size()==2 && sim.model.grid.at(storage::Location::LeftRifle), "Interior stash is independent of the physical mount");
    sim.returnToBack(); sim.pull();
    require(sim.model.items.size()==2 && sim.model.grid.at(storage::Location::LeftRifle), "Return and repull preserve all items and mounts");
    require(ui::savePreviewInventory(sim.model,path), "Simulator save failed");
    auto restored=ui::loadPreviewInventory(path);
    require(restored && restored->items.size()==2 && restored->grid.at(storage::Location::LeftRifle), "Simulator save/load preserves physical mounts and grid");
    preview::Simulation reload(std::move(*restored)); reload.takeSample(storage::Kind::Pistol);
    require(reload.hands[0]->item.storageId>sim.model.items.back().storageId, "Reload allocates a distinct sample identity");
    ui::Model list; list.available=true; list.generation=1; list.grid=storage::Grid(1,1);
    list.grid.insert({1,storage::Kind::Aid});
    list.items.push_back({.id="1",.name="Stimpak",.count=1,.storageId=1});
    for(unsigned id=2;id<602;++id) {
        require(list.grid.insert({id,storage::Kind::Misc})==storage::Result::Ok,"misc rejected on a full board");
        list.items.push_back({.id=std::to_string(id),.name=id%2?"Holotape":"Bobblehead",.category=rock_backpack::inventory_policy::Category::Misc,.count=1,.storageId=id,.appearance=id});
    }
    list.indexItems();
    require(ui::savePreviewInventory(list,path),"large misc snapshot save failed");
    auto listReload=ui::loadPreviewInventory(path);
    require(listReload && listReload->miscellaneous.size()==600 && listReload->grid.spatialIndices().size()==1,"misc list did not roundtrip independently of the grid");
    preview::Simulation listSim(std::move(*listReload));
    listSim.takeSample(storage::Kind::Misc);
    require(listSim.canStash(storage::Location::Interior) && listSim.stash(storage::Location::Interior),"misc deposit must ignore a full grid");
    require(listSim.draw(601) && listSim.hands[0].has_value(),"misc list item cannot be retrieved");
    { std::ofstream file(path,std::ios::trunc); file<<"{\"schema\":1,\"columns\":10,\"rows\":8,\"items\":[]}"; }
    require(!ui::loadPreviewInventory(path),"retired test inventories must not be imported");
    std::filesystem::remove(path);
}
