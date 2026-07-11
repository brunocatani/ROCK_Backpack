#pragma once
#include "Scene3D.h"
#include "Simulation.h"
#include "BackpackZones.h"
#include "BackpackConfig.h"
#include "ui/PanelView.h"
#include "ui/PointerPressGate.h"
#include <imgui_impl_dx11.h>

namespace preview
{
    class Surface
    {
    public:
        ~Surface() { if(_context) { auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(_context); if(ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext(_context); ImGui::SetCurrentContext(previous); } }
        bool initialize(ID3D11Device*,ID3D11DeviceContext*,unsigned width,unsigned height);
        std::optional<ui::Action> draw(ID3D11DeviceContext*,Simulation&,bool button,std::optional<Hit>,bool down,float delta,std::span<const ui::ItemPicture>,const ui::ItemImage*,const rock_backpack::zones::Guidance* guidance=nullptr,storage::Location location=storage::Location::Interior);
        ID3D11ShaderResourceView* image() const { return _image.Get(); }
    private:
        ImGuiContext* _context{};
        unsigned _width{},_height{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _image;
        ui::ViewState _view;
        ui::PointerPressGate _gate;
    };
    class OfflineScene
    {
    public:
        bool initialize(ID3D11Device*,ID3D11DeviceContext*,const std::filesystem::path&,unsigned width,unsigned height);
        bool resize(ID3D11Device* device,unsigned width,unsigned height) { return _scene.resize(device,width,height); }
        void configure(rock_backpack::BackpackSettings settings) { _settings=settings; }
        void front() { _yaw=_pitch=0; }
        void isometric() { _yaw=.6f; _pitch=.25f; }
        void editMount(unsigned index,bool rotate=false) { _editMounts=true; _editTarget=0; _selectedMount=int((std::min)(index,2u)); _rotateWeapon=rotate; }
        void editStash(unsigned index,bool resize) { _editMounts=true; _editTarget=1; _selectedStash=int((std::min)(index,3u)); _resizeStash=resize; }
        math::Vec3 mountPosition(unsigned index) const { return _settings.mounts.positions.at(index); }
        math::Vec3 mountAxisScreen() const { return _scene.screen(_settings.mounts.positions[unsigned(_selectedMount)]+math::Vec3{8,0,0}); }
        const rock_backpack::BackpackSettings& settings() const { return _settings; }
        math::Vec3 project(math::Vec3 point) const { return _scene.screen(point); }
        math::Vec3 depositScreen() const { return _scene.screen(_settings.stash.centers[unsigned(storage::Location::LeftRifle)]); }
        math::Vec3 buttonScreen() const { const math::StoredTransform root{{1,0,0,0,1,0,0,0,1},{},1}; return _scene.screen(math::computeBackpackPanelPose(root,_settings.buttonOffset)->position); }
        void frame(ID3D11DeviceContext*,ID3D11RenderTargetView*,Simulation&,std::span<const ui::ItemPicture>,const ui::ItemImage*);
    private:
        void wireBox(ImDrawList*,math::Vec3,math::Vec3,ImU32,float thickness=1);
        bool mountGizmo();
        bool rotationGizmo();
        Scene3D _scene;
        Surface _button,_inventory;
        std::array<Surface,4> _guides;
        rock_backpack::BackpackSettings _settings;
        float _yaw{.25f},_pitch{.15f},_distance{210};
        bool _zones{};
        bool _editMounts{};
        int _selectedMount{}, _dragAxis{-1};
        int _editTarget{},_selectedStash{};
        bool _resizeStash{};
        bool _rotateWeapon{};
        float _rotationLastAngle{};
    };
}
