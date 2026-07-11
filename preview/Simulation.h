#pragma once
#include "ui/PanelModel.h"
#include "StorageGesture.h"
#include <optional>
#include <array>

namespace preview
{
    namespace ui = rock_backpack::ui;
    namespace storage = rock_backpack::storage;
    struct Simulation
    {
        ui::Model model;
        struct Held { ui::Item item; storage::Kind kind{}; };
        std::array<std::optional<Held>,2> hands;
        std::array<storage::DeliveryGesture,2> gestures;
        unsigned hand{};
        bool onBack{}, open{};
        bool bagInRightHand{};
        float inventoryFullSeconds{};
        std::uint64_t next{10000};
        explicit Simulation(ui::Model initial) : model(std::move(initial)) { syncIds(); }
        void syncIds() { for (const auto& item : model.items) next=(std::max)(next,item.storageId+1); }
        void changed() { syncIds(); ++model.generation; model.indexItems(); }
        void takeSample(storage::Kind kind)
        {
            if(hands[hand]) { model.status="That hand already holds an item."; return; }
            const auto id=next++;
            Held value; value.kind=kind; value.item.storageId=id; value.item.id=std::to_string(id);
            switch(kind) {
            case storage::Kind::LongGun: value.item.name="Sample rifle"; break;
            case storage::Kind::Pistol: value.item.name="Sample pistol"; break;
            case storage::Kind::Sniper: value.item.name="Sample sniper rifle"; break;
            case storage::Kind::Heavy: value.item.name="Sample heavy weapon"; break;
            case storage::Kind::Melee: value.item.name="Sample melee weapon"; break;
            case storage::Kind::Knife: value.item.name="Sample knife"; break;
            case storage::Kind::Misc: value.item.name="Sample holotape"; break;
            case storage::Kind::Throwable: value.item.name="Fragmentation Grenade"; break;
            default: value.item.name="Stimpak"; break;
            }
            value.item.category=kind==storage::Kind::Aid?rock_backpack::inventory_policy::Category::Aid:
                kind==storage::Kind::Misc?rock_backpack::inventory_policy::Category::Misc:rock_backpack::inventory_policy::Category::Weapons;
            value.item.count=1; value.item.usable=kind==storage::Kind::Aid;
            value.item.formId=kind==storage::Kind::Aid?0x23736:kind==storage::Kind::Pistol?0x4822:kind==storage::Kind::Throwable?0xEEBED:0;
            hands[hand]=std::move(value); gestures[hand]={};
            model.status="Hold left mouse over a zone, then release to store the held sample.";
        }
        bool draw(storage::ItemId id)
        {
            if(onBack || hands[hand]) return false;
            const auto* entry=model.grid.find(id); if(!entry || entry->busy) return false;
            const auto found=std::find_if(model.items.begin(),model.items.end(),[id](const auto& item){return item.storageId==id;});
            if(found==model.items.end()) return false;
            Held held{*found,entry->kind}; held.item.count=1;
            held.item.storageId=next++; held.item.id=std::to_string(held.item.storageId);
            gestures[hand].delivered(unsigned(held.item.storageId)); hands[hand]=std::move(held);
            if(found->count>1) { --found->count; model.grid.setCount(id,found->count); }
            else { model.grid.erase(id); model.items.erase(found); }
            const bool weapon=storage::isWeapon(hands[hand]->kind);
            model.status=weapon?"Weapon equipped (simulated). Its draw release cannot restash it.":"Item drawn into the selected hand. Its draw release cannot restash it.";
            changed(); return true;
        }
        bool canStash(storage::Location zone) const
        {
            if(onBack || !hands[hand] || gestures[hand].reference) return false;
            const auto kind=hands[hand]->kind;
            if(zone==storage::Location::Interior && (kind==storage::Kind::Aid || kind==storage::Kind::Throwable))
                for(const auto& entry:model.grid.entries()) if(entry.placement.location==zone && !entry.busy && entry.stackKey==hands[hand]->item.formId) return true;
            return zone==storage::Location::Interior?bool(model.grid.firstFit(kind)):model.grid.fits(kind,{zone})==storage::Result::Ok;
        }
        bool stash(storage::Location zone)
        {
            if(onBack || !hands[hand]) return false;
            const auto held=*hands[hand];
            if(zone==storage::Location::Interior && (held.kind==storage::Kind::Aid || held.kind==storage::Kind::Throwable)) {
                for(auto& item:model.items) if(const auto* entry=model.grid.find(item.storageId);
                    entry && entry->placement.location==zone && !entry->busy && entry->stackKey==held.item.formId) {
                    ++item.count; model.grid.setCount(item.storageId,item.count); hands[hand].reset(); changed(); return true;
                }
            }
            auto placement=zone==storage::Location::Interior?model.grid.firstFit(held.kind):std::optional<storage::Placement>{{zone}};
            if(!placement || model.grid.insert({held.item.storageId,held.kind,held.item.count,
                held.kind==storage::Kind::Aid || held.kind==storage::Kind::Throwable?held.item.formId:0,*placement})!=storage::Result::Ok) {
                inventoryFullSeconds=2.2f; model.status="Inventory full. Item stays in your hand."; return false;
            }
            model.items.push_back(held.item); hands[hand].reset();
            model.status="Stored directly from the hand; no inventory panel was needed.";
            changed(); return true;
        }
        void input(std::optional<storage::Location> zone,bool down,bool pressed,bool released)
        {
            if(onBack) return;
            const unsigned ref=hands[hand]?unsigned(hands[hand]->item.storageId):0;
            const bool permitted=gestures[hand].permits(ref,true,down,pressed);
            if(hands[hand]) { if(zone && released && permitted) stash(*zone); }
            else if(zone && pressed && *zone!=storage::Location::Interior)
                if(const auto* entry=model.grid.at(*zone)) draw(entry->id);
        }
        void returnToBack() { onBack=true; open=false; model.status="Backpack on back. All mounts and contents retained."; }
        void pull() { onBack=false; model.status="Same backpack retrieved. Mounts and contents retained."; }
        void apply(const ui::Action& action)
        {
            if(action.kind==ui::ActionKind::Open) { open=!open; return; }
            if(action.kind==ui::ActionKind::Close) { open=false; return; }
            if(action.generation!=model.generation) return;
            auto found=std::find_if(model.items.begin(),model.items.end(),[&](const auto& item){return item.id==action.rowId.data();});
            if(found==model.items.end()) return;
            const auto id=found->storageId;
            if(action.kind==ui::ActionKind::Grab) { draw(id); return; }
            storage::Result result=storage::Result::Ok;
            if(action.kind==ui::ActionKind::Move) result=model.grid.move(id,action.destination);
            else if(action.kind==ui::ActionKind::Merge) {
                result=model.grid.merge(id,action.target);
                if(result==storage::Result::Ok) { model.items.erase(found); for(auto& item:model.items) if(item.storageId==action.target) item.count=model.grid.find(action.target)->count; }
            } else if(action.kind==ui::ActionKind::Split) {
                auto copy=*found; copy.storageId=next++; copy.id=std::to_string(copy.storageId); copy.count=action.quantity;
                result=model.grid.split(id,copy.storageId,action.quantity,action.destination);
                if(result==storage::Result::Ok) { found->count-=action.quantity; model.items.push_back(copy); }
            } else {
                if(found->count>1) { --found->count; model.grid.setCount(id,found->count); }
                else { model.grid.erase(id); model.items.erase(found); }
            }
            model.status=result==storage::Result::Ok?"Desktop item action completed.":"That layout or stack operation is not valid.";
            changed();
        }
    };
}
