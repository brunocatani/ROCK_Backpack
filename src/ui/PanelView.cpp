#include "PanelView.h"
#include "Theme.h"
#include <RPS_UI_Framework/ThemeStyle.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace rock_backpack::ui
{
    // RPS UI has already styled the panel's context with the theme and made its
    // Interface face the default; the desktop preview gets the same style here.
    void initializeStyle()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        rpsui::imgui_host::applyTheme(theme::current());
    }
    namespace
    {
        // Theme roles.
        ImU32 ink() { return theme::packed(theme::current().text); }
        ImU32 muted() { return theme::packed(theme::current().textMuted); }
        ImU32 edge() { return theme::packed(theme::current().border); }
        ImU32 accent() { return theme::packed(theme::current().accent); }
        struct Rect {
            ImVec2 min, max;
            bool contains(ImVec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
        };
        const Item* itemFor(const Model& model, storage::ItemId id)
        {
            const auto it = model.itemByStorageId.find(id);
            return it == model.itemByStorageId.end() ? nullptr : &model.items[it->second];
        }
        void centered(ImDrawList* draw, Rect rect, const char* text, ImU32 color = muted())
        {
            const auto size = ImGui::CalcTextSize(text);
            draw->AddText({(rect.min.x + rect.max.x - size.x) * .5f, (rect.min.y + rect.max.y - size.y) * .5f}, color, text);
        }
    }

    bool drawInventoryButton(float inventoryFullSeconds)
    {
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({float(kButtonWidth),float(kButtonHeight)});
        ImGui::Begin("Backpack inventory control", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPos({8,8}); ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,12.f);
        const bool pressed = ImGui::Button("##Inventory", {float(kButtonWidth-16),float(kButtonHeight-16)});
        const auto color = ImGui::IsItemHovered() ? accent() : ink();
        auto* draw = ImGui::GetWindowDrawList();
        auto* font = ImGui::GetFont(); const float size = 42.f;
        const auto text = font->CalcTextSizeA(size,FLT_MAX,0,"INVENTORY");
        draw->AddText(font,size,{(kButtonWidth-text.x)*.5f,(kButtonHeight-text.y)*.5f-(inventoryFullSeconds>0?18.f:0.f)},color,"INVENTORY");
        if(inventoryFullSeconds>0) {
            const auto notice=font->CalcTextSizeA(24.f,FLT_MAX,0,"INVENTORY FULL");
            const int alpha=int(255*std::clamp(inventoryFullSeconds/.35f,0.f,1.f));
            draw->AddText(font,24.f,{(kButtonWidth-notice.x)*.5f,float(kButtonHeight)-48},theme::packed(theme::current().pending,alpha/255.f),"INVENTORY FULL");
        }
        draw->AddRect({8,8},{float(kButtonWidth-8),float(kButtonHeight-8)},color,12.f,3.f,ImDrawFlags_None);
        ImGui::PopStyleVar(); ImGui::End();
        const auto mouse = ImGui::GetIO().MousePos;
        if (mouse.x >= 0 && mouse.y >= 0 && mouse.x <= kButtonWidth && mouse.y <= kButtonHeight) {
            auto* foreground = ImGui::GetForegroundDrawList();
            foreground->AddCircleFilled(mouse,12,theme::packed(theme::current().background,.9f));
            foreground->AddCircle(mouse,9,ink(),24,3);
            foreground->AddCircleFilled(mouse,3,ink());
        }
        return pressed;
    }
    std::optional<Action> drawPanel(const Model& model, ViewState& state,
        const IconTextures&, Hand hand, const ItemImage* stimpak, const char*, std::span<const ItemPicture> pictures)
    {
        std::optional<Action> action;
        if (state.dragHand != hand) {
            state.pressed = 0; state.dragging = false; state.dragHand = hand;
        }
        if (state.modelGeneration != model.generation) {
            state.pressed = 0; state.dragging = false; state.modelGeneration = model.generation;
            state.selectedIndex = model.items.size();
            for (std::size_t i = 0; i < model.items.size(); ++i) if (model.items[i].id == state.selected) state.selectedIndex = i;
        }
        const auto submit = [&](const Item* item, ActionKind kind, storage::Placement destination = {}, storage::ItemId target = 0, unsigned quantity = 1) {
            if (action || hand == Hand::None || !model.available || (item && (item->id.empty() || item->id.size() >= Action{}.rowId.size()))) return;
            Action value{.kind = kind, .generation = model.generation, .presentation = state.presentation, .hand = hand,
                .target = target, .destination = destination, .quantity = quantity};
            if (item) std::memcpy(value.rowId.data(), item->id.data(), item->id.size());
            action = value;
        };
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({float(kWidth), float(kHeight)});
        ImGui::Begin("Backpack", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddText({24, 18}, accent(), "MILITARY BACKPACK");
        ImGui::SetCursorPos({865, 12}); if (ImGui::Button("CLOSE", {110, 34})) submit(nullptr, ActionKind::Close);
        draw->AddLine({20, 54}, {980, 54}, edge(), 2);
        draw->AddText({24, 68}, ink(), "EXTERIOR MOUNTS");
        ImGui::SetCursorPos({380,64});
        const auto switchView=[&](bool miscellaneous) {
            state.miscellaneous=miscellaneous; state.overflow=false; state.heldWeapons=false; state.pressed=0; state.dragging=false;
            state.selected.clear(); state.selectedIndex=model.items.size();
        };
        if (ImGui::Button("GRID",{100,34})) switchView(false);
        if(state.dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            state.miscellaneous=state.overflow=state.heldWeapons=false;
        }
        ImGui::SameLine();
        char label[64]; std::snprintf(label,sizeof(label),"MISC (%zu)",model.miscellaneous.size());
        if (ImGui::Button(label,{140,34})) switchView(true);
        ImGui::SameLine(); std::snprintf(label,sizeof(label),"OVERFLOW (%zu)",model.overflow.size());
        if(ImGui::Button(label,{170,34})) { switchView(false); state.overflow=true; }
        ImGui::SameLine(); std::snprintf(label,sizeof(label),"IN HAND (%zu)",model.heldWeapons.size());
        if(ImGui::Button(label,{145,34})) { switchView(false); state.heldWeapons=true; }
        const Rect left{{24, 110}, {119, 399}}, right{{239, 110}, {334, 399}}, pistol{{24, 421}, {334, 510}};
        const ImVec2 origin{380, 110};
        const float cell = (std::min)(59.f, (std::min)(590.f / (std::max)(1u, model.grid.columns()), 424.f / (std::max)(1u, model.grid.rows())));
        const Rect board{origin, {origin.x + cell * model.grid.columns(), origin.y + cell * model.grid.rows()}};
        const auto mount = [&](Rect rect, const char* text) {
            draw->AddRectFilled(rect.min, rect.max, theme::packed(theme::current().background));
            draw->AddRect(rect.min, rect.max, edge(), 2); centered(draw, rect, text);
        };
        mount(left, "RIFLE L"); mount(right, "RIFLE R"); mount(pistol, "BOTTOM");
        centered(draw, {{122, 173}, {237, 211}}, "BACKPACK");
        centered(draw, {{122, 204}, {237, 242}}, "EXTERIOR");
        draw->AddLine({358, 68}, {358, 536}, edge());
        if (!state.miscellaneous && !state.overflow && !state.heldWeapons) for (unsigned y = 0; y < model.grid.rows(); ++y) for (unsigned x = 0; x < model.grid.columns(); ++x) {
            const ImVec2 p{origin.x + x * cell, origin.y + y * cell};
            draw->AddRectFilled(p, {p.x + cell, p.y + cell}, theme::packed(theme::current().surface));
            draw->AddRect(p, {p.x + cell, p.y + cell}, edge());
        }
        const auto rectangle = [&](const storage::Entry& entry) {
            switch (entry.placement.location) {
            case storage::Location::LeftRifle: return left;
            case storage::Location::RightRifle: return right;
            case storage::Location::Pistol: return pistol;
            default:
                const auto size = storage::footprint(entry.kind, entry.placement.rotated);
                const ImVec2 p{origin.x + cell * entry.placement.x, origin.y + cell * entry.placement.y};
                return Rect{p, {p.x + cell * size.width, p.y + cell * size.height}};
            }
        };
        const auto imageFor = [&](const Item& item) -> const ItemImage* {
            for (const auto& picture : pictures) if (picture.appearance == item.appearance && picture.image) return picture.image.get();
            return item.formId == kStimpakForm ? stimpak : nullptr;
        };
        const auto pointer = ImGui::GetIO().MousePos;
        const storage::Entry* hovered{};
        for (const auto index : model.grid.spatialIndices()) {
            const auto& entry = model.grid.entries()[index];
            if ((state.miscellaneous || state.overflow || state.heldWeapons) && entry.placement.location == storage::Location::Interior) continue;
            const auto* item = itemFor(model, entry.id); if (!item) continue;
            const auto rect = rectangle(entry);
            draw->AddRectFilled({rect.min.x + 2, rect.min.y + 2}, {rect.max.x - 2, rect.max.y - 2},
                entry.busy ? theme::packed(theme::current().pending, .2f) : theme::packed(theme::current().surface));
            const auto* image = imageFor(*item);
            if (image && image->view && image->width && image->height) {
                const bool turn = entry.placement.rotated || entry.placement.location == storage::Location::LeftRifle || entry.placement.location == storage::Location::RightRifle;
                const auto width = turn ? image->height : image->width, height = turn ? image->width : image->height;
                const float scale = (std::min)((rect.max.x - rect.min.x - 8) / width, (rect.max.y - rect.min.y - 8) / height);
                const ImVec2 p{(rect.min.x + rect.max.x - width * scale) * .5f, (rect.min.y + rect.max.y - height * scale) * .5f};
                const ImVec2 end{p.x + width * scale, p.y + height * scale};
                if (turn) draw->AddImageQuad(reinterpret_cast<ImTextureID>(image->view.Get()), p, {end.x,p.y}, end, {p.x,end.y}, {0,1}, {0,0}, {1,0}, {1,1});
                else draw->AddImage(reinterpret_cast<ImTextureID>(image->view.Get()), p, end);
            } else {
                // Missing previews keep the actual name; never substitute an unrelated item icon.
                draw->PushClipRect({rect.min.x + 5, rect.min.y + 4}, {rect.max.x - 5, rect.max.y - 4}, true);
                draw->AddText(ImGui::GetFont(), 13.f, {rect.min.x + 5, rect.min.y + 5}, muted(), item->name.c_str(), nullptr, rect.max.x - rect.min.x - 10);
                draw->PopClipRect();
            }
            if (entry.count > 1) {
                char number[24]; std::snprintf(number, sizeof(number), "%u", entry.count);
                const auto extent = ImGui::CalcTextSize(number);
                draw->AddText({rect.max.x - extent.x - 5, rect.max.y - extent.y - 3}, ink(), number);
            }
            draw->AddRect(rect.min, rect.max, state.selected == item->id ? accent() : edge(), 1, state.selected == item->id ? 2.f : 1.f);
            if (rect.contains(pointer)) hovered = &entry;
        }
        if (state.miscellaneous || state.overflow || state.heldWeapons) {
            const auto& rows=state.heldWeapons?model.heldWeapons:state.overflow?model.overflow:model.miscellaneous;
            ImGui::SetCursorPos(origin);
            ImGui::BeginChild(state.heldWeapons?"Equipped weapons":state.overflow?"Overflow items":"Miscellaneous items",{590,424},ImGuiChildFlags_Borders);
            if (rows.empty()) ImGui::TextWrapped(state.heldWeapons?"Equipped weapons do not use interior space. Stowing requires room.":state.overflow?"No overflow. Existing items and forced rewards that do not fit appear here.":"Holotapes, bobbleheads and other miscellaneous items appear here.");
            ImGuiListClipper clipper;
            clipper.Begin(int(rows.size()),48.f);
            while (clipper.Step()) for (int row=clipper.DisplayStart; row<clipper.DisplayEnd; ++row) {
                const auto itemIndex=rows[std::size_t(row)];
                const auto& item=model.items[itemIndex];
                ImGui::PushID(item.id.c_str());
                const auto p=ImGui::GetCursorScreenPos();
                (void)ImGui::Selectable("##item",state.selected==item.id,0,{0,44});
                if(ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    state.selected=item.id; state.selectedIndex=itemIndex;
                    if(const auto* entry=model.grid.find(item.storageId);entry && !entry->busy && entry->placement.location!=storage::Location::Equipped) {
                        state.pressed=entry->id; state.pressPosition=pointer; state.rotated=entry->placement.rotated;
                        state.offsetX=state.offsetY=0;
                    }
                }
                auto* listDraw=ImGui::GetWindowDrawList();
                const auto* image=imageFor(item);
                if (image && image->view && image->width && image->height) {
                    const float scale=40.f/(std::max)(image->width,image->height);
                    listDraw->AddImage(reinterpret_cast<ImTextureID>(image->view.Get()),p,{p.x+image->width*scale,p.y+image->height*scale});
                }
                const float endX=p.x+ImGui::GetContentRegionAvail().x;
                listDraw->PushClipRect({p.x+48,p.y},{endX-75,p.y+44},true);
                listDraw->AddText({p.x+48,p.y+11},ink(),item.name.c_str()); listDraw->PopClipRect();
                char count[24]; std::snprintf(count,sizeof(count),"x%u",item.count);
                listDraw->AddText({endX-70,p.y+11},muted(),count);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        if (hovered && !state.dragging) if (const auto* item = itemFor(model, hovered->id)) {
            ImGui::BeginTooltip(); ImGui::TextUnformatted(item->name.c_str());
            ImGui::Text("%u  /  %.2f weight", hovered->count, item->weight * hovered->count);
            if (hovered->busy) ImGui::TextUnformatted("Transfer in progress");
            ImGui::EndTooltip();
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered->busy) {
            const auto* item = itemFor(model, hovered->id);
            state.selected = item->id; state.selectedIndex = static_cast<std::size_t>(item - model.items.data());
            state.pressed = hovered->id; state.pressPosition = pointer; state.rotated = hovered->placement.rotated;
            state.offsetX = state.offsetY = 0;
            if (hovered->placement.location == storage::Location::Interior) {
                state.offsetX = unsigned((pointer.x - origin.x) / cell) - hovered->placement.x;
                state.offsetY = unsigned((pointer.y - origin.y) / cell) - hovered->placement.y;
            }
        }
        if (state.pressed && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 5)) state.dragging = true;
        const auto* dragged = model.grid.find(state.pressed);
        if (state.dragging && dragged) {
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { state.rotated = !state.rotated; std::swap(state.offsetX, state.offsetY); }
            std::optional<storage::Placement> destination;
            if (left.contains(pointer)) destination = storage::Placement{storage::Location::LeftRifle};
            else if (right.contains(pointer)) destination = storage::Placement{storage::Location::RightRifle};
            else if (pistol.contains(pointer)) destination = storage::Placement{storage::Location::Pistol};
            else if (!state.miscellaneous && !state.overflow && !state.heldWeapons && board.contains(pointer)) {
                const auto x = unsigned((pointer.x - origin.x) / cell), y = unsigned((pointer.y - origin.y) / cell);
                if (x >= state.offsetX && y >= state.offsetY) destination = storage::Placement{storage::Location::Interior, x - state.offsetX, y - state.offsetY, state.rotated};
            }
            if (destination) {
                auto ghost = *dragged; ghost.placement = *destination;
                const bool legal = model.grid.fits(ghost.kind, *destination, ghost.id) == storage::Result::Ok &&
                    (destination->location == storage::Location::Interior || ghost.count == 1);
                const auto rect = rectangle(ghost);
                draw->AddRectFilled(rect.min, rect.max, legal ? theme::packed(theme::current().success, .25f) : theme::packed(theme::current().invalid, .3f));
                draw->AddRect(rect.min, rect.max, legal ? accent() : theme::packed(theme::current().invalid), 1.f, 2.f, ImDrawFlags_None);
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    if (legal) submit(itemFor(model, dragged->id), ActionKind::Move, *destination);
                    else if (hovered && hovered->id != dragged->id && hovered->stackKey && hovered->stackKey == dragged->stackKey &&
                        hovered->placement.location == storage::Location::Interior && dragged->placement.location == storage::Location::Interior)
                        submit(itemFor(model, dragged->id), ActionKind::Merge, {}, hovered->id);
                }
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { state.pressed = 0; state.dragging = false; }
        draw->AddLine({20, 552}, {980, 552}, edge(), 2);
        ImGui::SetCursorPos({24, 568});
        const Item* selected = state.selectedIndex < model.items.size() ? &model.items[state.selectedIndex] : nullptr;
        const auto* selectedEntry = selected ? model.grid.find(selected->storageId) : nullptr;
        if (selected && selectedEntry) {
            ImGui::TextUnformatted(selected->name.c_str());
            ImGui::TextDisabled("Quantity %u   Weight %.2f", selectedEntry->count, selected->weight * selectedEntry->count);
            ImGui::BeginDisabled(selectedEntry->busy || state.dragging);
            const bool weapon = storage::isWeapon(selectedEntry->kind);
            if (ImGui::Button(weapon ? (selected->equipped?"UNEQUIP":"EQUIP") : "GRAB", {94, 38})) submit(selected, selected->equipped && weapon?ActionKind::ToggleWeapon:ActionKind::Grab);
            ImGui::SameLine(); if (ImGui::Button("DROP", {94, 38})) submit(selected, ActionKind::Drop);
            if (selected->usable) { ImGui::SameLine(); if (ImGui::Button("USE", {94, 38})) submit(selected, ActionKind::Use); }
            if (selectedEntry->kind == storage::Kind::Armor) { ImGui::SameLine(); if (ImGui::Button(selected->equipped?"UNEQUIP":"EQUIP", {94, 38})) submit(selected, selected->equipped?ActionKind::UnequipArmor:ActionKind::EquipArmor); }
            if(selectedEntry->placement.location==storage::Location::Overflow) {
                const auto destination=model.grid.firstFit(selectedEntry->kind);
                ImGui::SameLine(); ImGui::BeginDisabled(!destination);
                if(ImGui::Button("TO BAG",{94,38}) && destination) submit(selected,ActionKind::Move,*destination);
                ImGui::EndDisabled();
            }
            if(weapon && !selected->equipped) {
                ImGui::SameLine(); if(ImGui::Button("PLACE...",{94,38})) ImGui::OpenPopup("Place weapon");
                if(ImGui::BeginPopup("Place weapon")) {
                    constexpr std::array<const char*,3> names{"Left backpack mount","Right backpack mount","Backpack bottom mount"};
                    for(unsigned i=0;i<names.size();++i) {
                        const storage::Placement destination{storage::Location(i+1)};
                        const bool fits=model.grid.fits(selectedEntry->kind,destination,selectedEntry->id)==storage::Result::Ok;
                        ImGui::BeginDisabled(!fits);
                        if(ImGui::Selectable(names[i])) { submit(selected,ActionKind::Move,destination); ImGui::CloseCurrentPopup(); }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndPopup();
                }
            }
            if (selectedEntry->placement.location == storage::Location::Interior && storage::footprint(selectedEntry->kind).width > 1) {
                ImGui::SameLine(); if (ImGui::Button("ROTATE", {94, 38})) {
                    auto destination = selectedEntry->placement; destination.rotated = !destination.rotated;
                    submit(selected, ActionKind::Move, destination);
                }
            }
            if (selectedEntry->count > 1 && selectedEntry->stackKey) {
                ImGui::SameLine(); if (ImGui::Button("SPLIT", {94, 38})) { state.splitQuantity = 1; ImGui::OpenPopup("Split stack"); }
                if (ImGui::BeginPopup("Split stack")) {
                    ImGui::InputInt("Quantity", &state.splitQuantity);
                    state.splitQuantity=std::clamp(state.splitQuantity,1,int((std::min)(selectedEntry->count-1,std::uint32_t(INT_MAX))));
                    const auto free = model.grid.firstFit(selectedEntry->kind);
                    ImGui::BeginDisabled(!free);
                    if (ImGui::Button(selectedEntry->kind==storage::Kind::Misc?"Split stack":"Split into free cells") && free) { submit(selected, ActionKind::Split, *free, 0, unsigned(state.splitQuantity)); ImGui::CloseCurrentPopup(); }
                    ImGui::EndDisabled(); ImGui::EndPopup();
                }
            }
            ImGui::EndDisabled();
        } else {
            ImGui::TextUnformatted("Release an item inside the backpack to store it.");
            ImGui::TextDisabled("Select to inspect. Drag to organize. Rifles on the sides; pistol below.");
        }
        ImGui::SetCursorPos({24, 690}); ImGui::PushTextWrapPos(975); ImGui::TextDisabled("%s", model.status.c_str()); ImGui::PopTextWrapPos();
        ImGui::End(); return action;
    }
}
