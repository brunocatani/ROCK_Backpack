#include "OfflineScene.h"
#include "ui/PreviewInventory.h"
#include "ui/ZoneGuidance.h"
#include <cfloat>

namespace preview
{
    bool Surface::initialize(ID3D11Device* device,ID3D11DeviceContext* context,unsigned width,unsigned height)
    {
        _width=width; _height=height;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&_texture)) || FAILED(device->CreateRenderTargetView(_texture.Get(),nullptr,&_target)) ||
            FAILED(device->CreateShaderResourceView(_texture.Get(),nullptr,&_image))) return false;
        auto* previous=ImGui::GetCurrentContext(); _context=ImGui::CreateContext(); ImGui::SetCurrentContext(_context);
        ui::initializeStyle(); const bool ok=ImGui_ImplDX11_Init(device,context);
        _view.presentation=1; ImGui::SetCurrentContext(previous); return ok;
    }
    std::optional<ui::Action> Surface::draw(ID3D11DeviceContext* context,Simulation& simulation,bool button,std::optional<Hit> hit,bool down,float delta,
        std::span<const ui::ItemPicture> pictures,const ui::ItemImage* stimpak,const rock_backpack::zones::Guidance* guidance,storage::Location location)
    {
        auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(_context);
        auto& io=ImGui::GetIO(); io.DisplaySize={float(_width),float(_height)}; io.DeltaTime=delta; io.MouseDrawCursor=!button && !guidance;
        const auto hand=hit?(simulation.hand?ui::Hand::Left:ui::Hand::Right):ui::Hand::None;
        io.AddMousePosEvent(hit?hit->x*_width:-FLT_MAX,hit?hit->y*_height:-FLT_MAX);
        io.AddMouseButtonEvent(0,_gate.update(1,hand,down));
        ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
        std::optional<ui::Action> action;
        if(guidance) ui::drawZoneGuidance(*guidance,location);
        else if(button) { if(ui::drawInventoryButton(simulation.inventoryFullSeconds)) action=ui::Action{.kind=ui::ActionKind::Open}; }
        else action=ui::drawPanel(simulation.model,_view,{},hand,stimpak,"Desktop capture",pictures);
        ImGui::Render(); auto* target=_target.Get();
        ID3D11ShaderResourceView* empty{}; context->PSSetShaderResources(0,1,&empty);
        context->OMSetRenderTargets(1,&target,nullptr); const float clear[]{.045f,.06f,.04f,1}, transparent[]{0,0,0,0};
        context->ClearRenderTargetView(target,guidance?transparent:clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); ImGui::SetCurrentContext(previous);
        return action;
    }
    bool OfflineScene::initialize(ID3D11Device* device,ID3D11DeviceContext* context,const std::filesystem::path& assets,unsigned width,unsigned height)
    {
        if(!_scene.initialize(device,assets,width,height) || !_button.initialize(device,context,ui::kButtonWidth,ui::kButtonHeight) ||
            !_inventory.initialize(device,context,ui::kWidth,ui::kHeight)) return false;
        for(auto& guide:_guides) if(!guide.initialize(device,context,unsigned(ui::kGuidanceWidth),unsigned(ui::kGuidanceHeight))) return false;
        return true;
    }
    void OfflineScene::wireBox(ImDrawList* draw,math::Vec3 center,math::Vec3 half,ImU32 color,float thickness)
    {
        std::array<ImVec2,8> vertices;
        for(unsigned i=0;i<8;++i) { const auto p=_scene.screen(center+math::Vec3{i&1?half.x:-half.x,i&2?half.y:-half.y,i&4?half.z:-half.z}); vertices[i]={p.x,p.y}; }
        for(unsigned i=0;i<8;++i) for(unsigned axis=0;axis<3;++axis) if(!(i&(1u<<axis))) draw->AddLine(vertices[i],vertices[i|(1u<<axis)],color,thickness);
    }
    bool OfflineScene::rotationGizmo()
    {
        auto& io=ImGui::GetIO(); auto* draw=ImGui::GetForegroundDrawList();
        const auto pivot=_settings.mounts.positions[unsigned(_selectedMount)];
        auto& angles=_settings.mounts.rotations[unsigned(_selectedMount)];
        const math::Vec3 axes[]{{1,0,0},{0,1,0},{0,0,1}};
        const ImU32 colors[]{IM_COL32(242,99,87,255),IM_COL32(111,219,118,255),IM_COL32(95,170,252,255)};
        const char* labels[]{"X","Y","Z"};
        const auto ray=_scene.ray(io.MousePos.x,io.MousePos.y);
        bool hovered=false; const bool captured=_dragAxis>=0;
        for(unsigned axis=0;axis<3;++axis) {
            // Euler rings use the same trailing rotations as visualRotation.
            const auto orient=[&](math::Vec3 p) {
                const float y=angles.y*.01745329252f,z=angles.z*.01745329252f;
                if(axis==0) p={std::cos(y)*p.x+std::sin(y)*p.z,p.y,-std::sin(y)*p.x+std::cos(y)*p.z};
                if(axis<2) p={std::cos(z)*p.x-std::sin(z)*p.y,std::sin(z)*p.x+std::cos(z)*p.y,p.z};
                return p;
            };
            const auto u=orient(axes[(axis+1)%3]),v=orient(axes[(axis+2)%3]),normal=math::cross(u,v);
            const float denominator=math::dot(ray.direction,normal);
            const bool usable=std::abs(denominator)>.03f;
            const auto color=usable?colors[axis]:(colors[axis]&0x00ffffff)|0x60000000;
            bool hit=false;
            ImVec2 previous{};
            for(unsigned i=0;i<=64;++i) {
                const float angle=float(i)*6.283185307f/64;
                const auto p=_scene.screen(pivot+(u*std::cos(angle)+v*std::sin(angle))*9.f);
                const ImVec2 point{p.x,p.y};
                if(i) {
                    draw->AddLine(previous,point,color,_dragAxis==int(axis)?4.f:2.f);
                    const float dx=point.x-previous.x,dy=point.y-previous.y,length=dx*dx+dy*dy;
                    if(length>.01f) {
                        const float t=std::clamp(((io.MousePos.x-previous.x)*dx+(io.MousePos.y-previous.y)*dy)/length,0.f,1.f);
                        const float x=io.MousePos.x-previous.x-t*dx,y=io.MousePos.y-previous.y-t*dy;
                        hit|=usable && io.MousePos.x>320 && x*x+y*y<64;
                    }
                } else draw->AddText({point.x+7,point.y-10},color,labels[axis]);
                previous=point;
            }
            hovered|=hit;
            if(!usable) continue;
            const float distance=math::dot(pivot-ray.origin,normal)/denominator;
            if(distance<=0) continue;
            const auto delta=ray.origin+ray.direction*distance-pivot;
            const float angle=std::atan2(math::dot(delta,v),math::dot(delta,u));
            if(hit && _dragAxis<0 && ImGui::IsMouseClicked(0)) { _dragAxis=int(axis); _rotationLastAngle=angle; }
            if(_dragAxis==int(axis) && ImGui::IsMouseDown(0)) {
                const float change=std::remainder(angle-_rotationLastAngle,6.283185307f)*57.29577951f;
                float* values[]{&angles.x,&angles.y,&angles.z};
                *values[axis]=std::clamp(*values[axis]+change,-360.f,360.f); _rotationLastAngle=angle;
            }
        }
        if(!ImGui::IsMouseDown(0)) _dragAxis=-1;
        return captured || hovered || _dragAxis>=0;
    }

    bool OfflineScene::mountGizmo()
    {
        if(_editTarget==0 && _rotateWeapon) return rotationGizmo();
        auto& io=ImGui::GetIO(); auto* draw=ImGui::GetForegroundDrawList();
        auto& point=_editTarget==0?_settings.mounts.positions[unsigned(_selectedMount)]:_settings.stash.centers[unsigned(_selectedStash)];
        auto& half=_settings.stash.halves[unsigned(_selectedStash)];
        const bool resize=_editTarget==1 && _resizeStash;
        float* sizes[]{&half.x,&half.y,&half.z};
        const auto origin=_scene.screen(point);
        const math::Vec3 axes[]{{1,0,0},{0,1,0},{0,0,1}};
        const ImU32 colors[]{IM_COL32(242,99,87,255),IM_COL32(111,219,118,255),IM_COL32(95,170,252,255)};
        const char* labels[]{"X","Y","Z"};
        bool hovered=false; const bool captured=_dragAxis>=0;
        for(unsigned axis=0;axis<3;++axis) {
            const float units=resize?*sizes[axis]:9.f;
            auto end=_scene.screen(point+axes[axis]*units);
            float dx=end.x-origin.x,dy=end.y-origin.y;
            // A depth axis projects to a point in the front view. Its diagonal
            // handle still permits depth editing; orbit shows the true axis.
            if(dx*dx+dy*dy<64) { dx=30; dy=-30; end={origin.x+dx,origin.y+dy,origin.z}; }
            const float squared=dx*dx+dy*dy;
            const float t=std::clamp(((io.MousePos.x-origin.x)*dx+(io.MousePos.y-origin.y)*dy)/squared,.2f,1.f);
            const float mx=io.MousePos.x-origin.x-dx*t,my=io.MousePos.y-origin.y-dy*t;
            const bool hit=io.MousePos.x>320 && mx*mx+my*my<100;
            hovered|=hit;
            if(hit && _dragAxis<0 && ImGui::IsMouseClicked(0)) _dragAxis=int(axis);
            if(_dragAxis==int(axis) && ImGui::IsMouseDown(0)) {
                const float change=(io.MouseDelta.x*dx+io.MouseDelta.y*dy)*units/squared;
                float* values[]{&point.x,&point.y,&point.z};
                if(resize) *sizes[axis]=std::clamp(*sizes[axis]+change,.5f,50.f);
                else *values[axis]=std::clamp(*values[axis]+change,-100.f,100.f);
            }
            const float length=std::sqrt(squared),ux=dx/length,uy=dy/length;
            draw->AddLine({origin.x,origin.y},{end.x,end.y},colors[axis],_dragAxis==int(axis)?5.f:3.f);
            if(resize) draw->AddRectFilled({end.x-5,end.y-5},{end.x+5,end.y+5},colors[axis]);
            else draw->AddTriangleFilled({end.x,end.y},{end.x-ux*13-uy*5,end.y-uy*13+ux*5},
                {end.x-ux*13+uy*5,end.y-uy*13-ux*5},colors[axis]);
            draw->AddText({end.x+7,end.y-10},colors[axis],labels[axis]);
        }
        draw->AddCircleFilled({origin.x,origin.y},5,IM_COL32(245,238,213,255));
        if(!ImGui::IsMouseDown(0)) _dragAxis=-1;
        return captured || hovered || _dragAxis>=0;
    }

    void OfflineScene::frame(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,Simulation& sim,
        std::span<const ui::ItemPicture> pictures,const ui::ItemImage* stimpak)
    {
        auto& io=ImGui::GetIO();
        sim.inventoryFullSeconds=(std::max)(0.f,sim.inventoryFullSeconds-io.DeltaTime);
        bool sceneInput=io.MousePos.x>320;
        if(sceneInput && ImGui::IsMouseDragging(1)) { _yaw+=io.MouseDelta.x*.008f; _pitch=std::clamp(_pitch+io.MouseDelta.y*.006f,-1.3f,1.3f); }
        if(sceneInput) _distance=std::clamp(_distance-io.MouseWheel*7.f,65.f,240.f);
        ImGui::SetNextWindowPos({12,12}); ImGui::SetNextWindowSize({295,(std::max)(300.f,io.DisplaySize.y-148.f)});
        ImGui::Begin("Backpack simulator",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse);
        ImGui::TextWrapped("Zero keeps the shipped calibration. Save applies your adjustments in game.");
        ImGui::Checkbox("Show interaction zones",&_zones);
        if(ImGui::Button("Front")) { _yaw=0; _pitch=0; }
        ImGui::SameLine(); if(ImGui::Button("Isometric")) { _yaw=.6f; _pitch=.25f; }
        ImGui::TextWrapped("Right mouse: orbit. Wheel: zoom.");
        ImGui::Separator();
        if(ImGui::CollapsingHeader("Simulation controls")) {
        if(ImGui::Button(sim.onBack?"Pull backpack":"Return backpack to back",{-1,32})) { if(sim.onBack) sim.pull(); else sim.returnToBack(); }
        ImGui::Checkbox("Hold backpack: right hand",&sim.bagInRightHand);
        if(ImGui::RadioButton("Right hand",sim.hand==0)) sim.hand=0;
        if(ImGui::RadioButton("Left hand",sim.hand==1)) sim.hand=1;
        constexpr storage::Kind samples[]{storage::Kind::LongGun,storage::Kind::Pistol,storage::Kind::Sniper,storage::Kind::Heavy,
            storage::Kind::Melee,storage::Kind::Knife,storage::Kind::Aid,storage::Kind::Misc};
        constexpr const char* labels[]{"Take sample rifle","Take sample pistol","Take sniper rifle","Take heavy weapon",
            "Take melee weapon","Take knife","Take Stimpak","Take holotape"};
        for(unsigned sample=0;sample<std::size(samples);++sample) {
            const auto kind=samples[sample]; const char* label=labels[sample];
            if(ImGui::Button(label,{-1,28})) sim.takeSample(kind);
        }
        if(sim.hands[sim.hand]) {
            ImGui::TextWrapped("Held: %s",sim.hands[sim.hand]->item.name.c_str());
            if(ImGui::Button("Return held sample to tray")) sim.hands[sim.hand].reset();
        } else ImGui::TextWrapped("Hand empty: click a grab region to draw.");
        ImGui::TextWrapped("To stash: hold left mouse inside a zone, then release. The panel can remain closed.");
        if(ImGui::Button("Empty sample backpack")) { sim.model.items.clear(); sim.model.grid=storage::Grid{}; sim.changed(); }
        const auto saved=ui::stimpakImagePath().parent_path()/"DesktopSimulation.json";
        if(ImGui::Button("Save layout")) sim.model.status=ui::savePreviewInventory(sim.model,saved)?"Desktop state saved.":"Desktop save failed.";
        ImGui::SameLine(); if(ImGui::Button("Reload")) if(auto loaded=ui::loadPreviewInventory(saved)) { sim.model=std::move(*loaded); sim.changed(); }
        }
        if(ImGui::Button("Reload runtime INI")) {
            std::string error;
            if(auto settings=rock_backpack::BackpackConfig::readOnly(error)) { _settings=*settings; sim.model.status="Loaded runtime panel and button placement."; }
            else sim.model.status="INI read failed: "+error;
        }
        const rock_backpack::BackpackSettings baseline;
        const auto inputVector=[](const char* label,math::Vec3& p,math::Vec3 base,float minimum,float maximum) {
            float values[]{p.x-base.x,p.y-base.y,p.z-base.z};
            ImGui::TextUnformatted(label); ImGui::PushID(label); ImGui::SetNextItemWidth(-1);
            if(ImGui::InputFloat3("##values",values,"%.2f")) {
                if(std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]))
                    p={std::clamp(base.x+values[0],minimum,maximum),std::clamp(base.y+values[1],minimum,maximum),std::clamp(base.z+values[2],minimum,maximum)};
            }
            ImGui::PopID();
        };
        const auto inputScalar=[](const char* label,float& actual,float base,float minimum,float maximum) {
            float adjustment=actual-base; ImGui::TextUnformatted(label); ImGui::PushID(label); ImGui::SetNextItemWidth(-1);
            if(ImGui::InputFloat("##value",&adjustment,.1f,1.f,"%.2f") && std::isfinite(adjustment)) actual=std::clamp(base+adjustment,minimum,maximum);
            ImGui::PopID();
        };
        if(ImGui::CollapsingHeader("Button placement")) {
            inputVector("Position adjustment",_settings.buttonOffset,baseline.buttonOffset,-100,100);
            inputScalar("Width adjustment",_settings.buttonWidth,baseline.buttonWidth,1,60);
            inputScalar("Height adjustment",_settings.buttonHeight,baseline.buttonHeight,1,60);
            if(ImGui::Button("Reset button")) { _settings.buttonOffset=baseline.buttonOffset; _settings.buttonWidth=baseline.buttonWidth; _settings.buttonHeight=baseline.buttonHeight; }

        }
        if(ImGui::CollapsingHeader("Weapon placement",ImGuiTreeNodeFlags_DefaultOpen)) {
            if(ImGui::Combo("Weapon",&_selectedMount,"Left rifle\0Right rifle\0Pistol\0")) _dragAxis=-1;
            ImGui::TextWrapped("Weapon visual only.");
            const auto i=unsigned(_selectedMount);
            inputVector("Position adjustment X/Y/Z",_settings.mounts.positions[i],baseline.mounts.positions[i],-100,100);
            inputVector("Angle adjustment X/Y/Z",_settings.mounts.rotations[i],baseline.mounts.rotations[i],-360,360);
            if(ImGui::Button("Weapon position arrows")) { editMount(i); _dragAxis=-1; }
            if(ImGui::Button("Weapon rotation rings")) { editMount(i,true); _dragAxis=-1; }
            if(ImGui::Button("Reset weapon")) { _settings.mounts.positions[i]=baseline.mounts.positions[i]; _settings.mounts.rotations[i]=baseline.mounts.rotations[i]; }
        }
        if(ImGui::CollapsingHeader("Stash regions",ImGuiTreeNodeFlags_DefaultOpen)) {
            if(ImGui::Combo("Region",&_selectedStash,"Interior\0Left rifle\0Right rifle\0Pistol\0")) _dragAxis=-1;
            const auto i=unsigned(_selectedStash);
            inputVector("Center adjustment X/Y/Z",_settings.stash.centers[i],baseline.stash.centers[i],-100,100);
            auto dimensions=_settings.stash.halves[i]*2.f;
            inputVector("Size adjustment W/H/D",dimensions,baseline.stash.halves[i]*2.f,1,100);
            _settings.stash.halves[i]=dimensions*.5f;
            ImGui::TextDisabled("Size: %.1f x %.1f x %.1f",dimensions.x,dimensions.y,dimensions.z);
            if(ImGui::Button("Region position arrows")) { editStash(i,false); _dragAxis=-1; }
            if(ImGui::Button("Region size handles")) { editStash(i,true); _dragAxis=-1; }
            if(ImGui::Button("Reset region")) {
                const rock_backpack::zones::StashLayout defaults;
                _settings.stash.centers[i]=defaults.centers[i]; _settings.stash.halves[i]=defaults.halves[i];
            }
            ImGui::TextWrapped("Indicator and grab region stay fixed.");
        }
        ImGui::Checkbox("Show editing handles",&_editMounts);
        ImGui::Separator(); ImGui::TextWrapped("%s",sim.model.status.c_str());
        ImGui::TextWrapped("Original game meshes with static base attachments. Game lighting and your weapon modifications may differ.");
        ImGui::TextDisabled("F3: grid / 3D view");
        ImGui::TextDisabled("F11 / Alt+Enter: fullscreen"); ImGui::End();
        ImGui::SetNextWindowPos({12,(std::max)(324.f,io.DisplaySize.y-124.f)});
        ImGui::SetNextWindowSize({295,112});
        ImGui::Begin("Save placement",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);
        inputScalar("Panel angle offset (deg)",_settings.panelYawDegrees,baseline.panelYawDegrees,-60,60);
        if(ImGui::Button("Save to game INI",{-1,30})) {
            std::string error;
            sim.model.status=rock_backpack::BackpackConfig::saveSettings(_settings,error)?
                "Saved. The game reloads the placement and regions automatically.":"Save failed: "+error;
        }
        ImGui::End();
        _scene.camera(_yaw,_pitch,_distance,sim.bagInRightHand);
        if(_editMounts && !sim.onBack) { if(mountGizmo()) sceneInput=false; }
        else _dragAxis=-1;
        const math::StoredTransform root{{1,0,0,0,1,0,0,0,1},{},1};
        const auto buttonPose=math::computeBackpackPanelPose(root,_settings.buttonOffset).value();
        const auto inventoryPose=math::anchoredInventoryPose(root,_settings.panelPhysicalWidth,_settings.localOffset,sim.bagInRightHand,_settings.panelYawDegrees).value();
        const auto ray=_scene.ray(io.MousePos.x,io.MousePos.y);
        const auto buttonHit=sceneInput && !sim.onBack?intersect(ray,buttonPose,_settings.buttonWidth,_settings.buttonHeight):std::nullopt;
        const auto inventoryHit=sceneInput && !sim.onBack && sim.open?intersect(ray,inventoryPose,_settings.panelPhysicalWidth,_settings.panelPhysicalHeight):std::nullopt;
        if(auto action=_button.draw(context,sim,true,buttonHit,io.MouseDown[0],io.DeltaTime,pictures,stimpak)) sim.apply(*action);
        if(auto action=_inventory.draw(context,sim,false,inventoryHit,io.MouseDown[0],io.DeltaTime,pictures,stimpak)) sim.apply(*action);
        namespace zones=rock_backpack::zones;
        std::optional<storage::Location> region; float nearest=10000;
        const bool depositing=sim.hands[sim.hand].has_value();
        const auto allowed=depositing?zones::depositMask(sim.hands[sim.hand]->kind):zones::drawMask(sim.model.grid);
        if(!sim.onBack && sceneInput && !buttonHit && !inventoryHit) for(const auto& box:zones::regions(depositing,_settings.stash))
            if(auto hit=intersectBox(ray,box.center,box.half); (allowed & (1u<<unsigned(box.location))) && hit && *hit<nearest) { nearest=*hit; region=box.location; }
        zones::Guidance guidance{depositing?allowed:15u,0,0};
        for(unsigned slot=1;slot<=3;++slot) if(sim.model.grid.at(storage::Location(slot))) guidance.blocked |= 1u<<slot;
        if(depositing && region && sim.canStash(*region)) guidance.ready |= 1u<<unsigned(*region);
        for(unsigned i=0;i<_guides.size();++i)
            (void)_guides[i].draw(context,sim,false,{},false,io.DeltaTime,pictures,stimpak,&guidance,storage::Location(i));
        _scene.begin(context,target);
        if(!sim.onBack) {
            _scene.backpack(context);
            _scene.panel(context,_button.image(),buttonPose,_settings.buttonWidth,_settings.buttonHeight);
            if(sim.open) _scene.panel(context,_inventory.image(),inventoryPose,_settings.panelPhysicalWidth,_settings.panelPhysicalHeight);
            for(unsigned i=0;i<_guides.size();++i) {
                const auto location=storage::Location(i);
                const auto box=zones::depositRegion(location);
                _scene.panel(context,_guides[i].image(),*zones::guidancePose(root,location),box.half.x*2,box.half.y*2,true);
            }
            auto* draw=ImGui::GetBackgroundDrawList();
            constexpr ImU32 colors[]{IM_COL32(97,197,141,150),IM_COL32(238,204,99,190),IM_COL32(112,183,233,190),IM_COL32(231,149,99,190)};
            const char* labels[]{"INTERNAL STASH","LEFT RIFLE","RIGHT RIFLE","PISTOL"};
            for(const auto& box:rock_backpack::zones::regions(depositing,_settings.stash)) if(_zones) {
                const bool hovered=region && *region==box.location;
                wireBox(draw,box.center,box.half,colors[unsigned(box.location)],hovered?3.f:1.f);
                const auto p=_scene.screen(box.center+math::Vec3{0,-box.half.y,box.half.z});
                draw->AddText({p.x,p.y},colors[unsigned(box.location)],labels[unsigned(box.location)]);
            }
            if(_editMounts && _editTarget==1)
                wireBox(draw,_settings.stash.centers[unsigned(_selectedStash)],_settings.stash.halves[unsigned(_selectedStash)],IM_COL32(222,192,113,240),2.f);
            for(unsigned slot=1;slot<=3;++slot) if(sim.model.grid.at(storage::Location(slot))) {
                const auto location=storage::Location(slot);
                const bool selected=(_editMounts && _editTarget==0 && int(slot)==_selectedMount+1) || (!depositing && region && *region==location);
                _scene.weapon(context,location,_settings.mounts,selected);
            }

        }
        sim.input(region,io.MouseDown[0],sceneInput && !buttonHit && !inventoryHit && ImGui::IsMouseClicked(0),sceneInput && !buttonHit && !inventoryHit && ImGui::IsMouseReleased(0));
        // ImGui overlays are submitted by the executable after this 3D pass.
    }
}
