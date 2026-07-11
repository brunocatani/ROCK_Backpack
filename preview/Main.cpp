#include "OfflineScene.h"
#include "ui/PanelView.h"
#include "ui/PreviewInventory.h"
#include "ui/PullPrompt.h"
#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    using Microsoft::WRL::ComPtr;
    namespace ui = rock_backpack::ui;
    using Category = rock_backpack::inventory_policy::Category;
    using Icon = palm::api::Icon;
    namespace storage = rock_backpack::storage;

    struct WindowState
    {
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
        bool fullscreen{};
        void toggle(HWND hwnd)
        {
            if (!fullscreen) {
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (!GetWindowPlacement(hwnd,&placement) || !GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)) return;
                SetWindowLongPtrW(hwnd,GWL_STYLE,GetWindowLongPtrW(hwnd,GWL_STYLE)&~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd,nullptr,monitor.rcMonitor.left,monitor.rcMonitor.top,
                    monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOZORDER|SWP_FRAMECHANGED);
            } else {
                SetWindowLongPtrW(hwnd,GWL_STYLE,GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_OVERLAPPEDWINDOW);
                SetWindowPlacement(hwnd,&placement);
                SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            }
            fullscreen=!fullscreen;
        }
    };

    LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message==WM_NCCREATE) SetWindowLongPtrW(hwnd,GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        auto* window=reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (window && (message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lparam&(1ll<<30)) &&
            (wparam==VK_F11 || (wparam==VK_RETURN && (lparam&(1ll<<29))) || (wparam==VK_ESCAPE && window->fullscreen))) {
            window->toggle(hwnd); return 0;
        }
        if (message==WM_GETMINMAXINFO) {
            auto* limits=reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize={720,540}; return 0;
        }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return true;
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        if (message == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_KEYMENU) return 0;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    ui::Model sampleInventory()
    {
        ui::Model model;
        model.generation = 1; model.available = true;
        model.vitalsAvailable = true; model.capacityKnown = true;
        model.weight = 87.5f; model.capacity = 210; model.health = 168; model.maxHealth = 200;
        model.status = "DESKTOP SIMULATION  /  Drag to arrange. R rotates while dragging. F5 resets the sample bag.";
        model.items = {
            { "10mm", "10mm Pistol", "", Category::Weapons, Icon::Pistol10mm, 1, 0, 3.5f },
            { "rifle", "Hunting Rifle", "", Category::Weapons, Icon::HuntingRifle, 1, 0, 11.2f },
            { "combat", "Combat Rifle", "", Category::Weapons, Icon::CombatRifle, 1, 0, 8.2f },
            { "shotgun", "Combat Shotgun", "", Category::Weapons, Icon::CombatShotgun, 1, 0, 12.4f },
            { "knife", "Combat Knife", "", Category::Weapons, Icon::CombatKnife, 1, 0, 1.0f },
            { "grenades", "Fragmentation Grenade", "", Category::Weapons, Icon::FragGrenade, 6, 0, .5f, false, false, 0xEEBED },
            { "molotov", "Molotov Cocktail", "", Category::Weapons, Icon::Molotov, 4, 0, .5f, false, false, 0x10C3C6 },
            { "pulse", "Pulse Grenade", "", Category::Weapons, Icon::FragGrenade, 3, 0, .5f, false, false, 0xFF21F },
            { "plasma", "Plasma Pistol", "", Category::Weapons, Icon::PlasmaPistol, 1, 0, 4.2f },
            { "minigun", "Minigun", "", Category::Weapons, Icon::Minigun, 1, 0, 27.4f },
            { "sniper", "Sniper Rifle", "", Category::Weapons, Icon::HuntingRifle, 1, 0, 12.4f },
            { "melee", "Baseball Bat", "", Category::Weapons, Icon::BaseballBat, 1, 0, 3.0f },
            { "helmet", "Combat Armor Helmet", "Head", Category::Armor, Icon::Helmet, 1, 1, 4.0f, true },
            { "chest", "Combat Armor Chest", "Torso", Category::Armor, Icon::ChestArmor, 1, 2, 8.0f, true },
            { "leftarm", "Leather Left Arm", "Left arm", Category::Armor, Icon::LeftArm, 1, 4, 2.0f, true },
            { "rightarm", "Leather Right Arm", "Right arm", Category::Armor, Icon::RightArm, 1, 8, 2.0f, true },
            { "legs", "Military Fatigues", "Torso / Arms / Legs", Category::Armor, Icon::Clothing, 1, 30, 3.0f, true },
            { "spare", "Metal Chest Piece", "Torso", Category::Armor, Icon::ChestArmor, 1, 2, 12.0f },
            { "stimpak", "Stimpak", "", Category::Aid, Icon::Stimpak, 14, 0, .1f, false, true, ui::kStimpakForm },
            { "radaway", "RadAway", "", Category::Aid, Icon::RadAway, 5, 0, .1f, false, true, 0x23742 },
            { "radx", "Rad-X", "", Category::Aid, Icon::RadX, 5, 0, .1f, false, true, 0x24057 },
            { "water", "Purified Water", "", Category::Aid, Icon::PurifiedWater, 7, 0, .5f, false, true },
            { "food", "Cram", "", Category::Aid, Icon::CannedFood, 3, 0, .5f, false, true },
            { "screws", "Screws", "", Category::Junk, Icon::Config, 22, 0, .1f },
            { "ammo", "10mm Rounds", "", Category::Ammo, Icon::Grenades, 180, 0, .01f },
            { "tape", "Overseer's Log", "", Category::Misc, Icon::OpenHand, 1, 0, 0, false, true },
            { "bobblehead", "Perception Bobblehead", "", Category::Misc, Icon::OpenHand, 1, 0, 0 },
        };
        std::uint64_t nextId = 1;
        for (auto& item : model.items) {
            item.storageId = nextId++;
            auto kind = item.category == Category::Armor ? storage::Kind::Armor :
                item.category == Category::Aid ? storage::Kind::Aid : item.category == Category::Ammo ? storage::Kind::Ammo :
                item.category == Category::Junk ? storage::Kind::Junk : storage::Kind::Misc;
            if (item.category == Category::Weapons) kind = storage::Kind::LongGun;
            if (item.id == "10mm" || item.id == "plasma") kind = storage::Kind::Pistol;
            if (item.id == "knife") kind = storage::Kind::Knife;
            if (item.id == "melee") kind = storage::Kind::Melee;
            if (item.id == "minigun") kind = storage::Kind::Heavy;
            if (item.id == "sniper") kind = storage::Kind::Sniper;
            if (item.id == "grenades" || item.id=="molotov" || item.id=="pulse") kind = storage::Kind::Throwable;
            auto placement = model.grid.firstFit(kind).value();
            if (item.id == "rifle") placement = {storage::Location::LeftRifle};
            if (item.id == "combat") placement = {storage::Location::RightRifle};
            if (item.id == "10mm") placement = {storage::Location::Pistol};
            // Sample-only compatibility keys. Production keys come from the native inventory backend.
            model.grid.insert({item.storageId, kind, item.count, item.count > 1 ? item.storageId : 0, placement});
        }
        model.indexItems();
        return model;
    }

    bool saveCapture(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Texture2D* back, const wchar_t* path)
    {
        D3D11_TEXTURE2D_DESC desc; back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        context->CopyResource(staging.Get(), back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        struct Unmap { ID3D11DeviceContext* context; ID3D11Texture2D* texture; ~Unmap() { context->Unmap(texture, 0); } } unmap{ context, staging.Get() };
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> converted;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(desc.Width, desc.Height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(factory->CreateBitmapFromMemory(desc.Width, desc.Height, GUID_WICPixelFormat32bppRGBA,
                mapped.RowPitch, mapped.RowPitch * desc.Height, static_cast<BYTE*>(mapped.pData), &bitmap)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converted)) &&
            SUCCEEDED(converted->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(frame->WriteSource(converted.Get(), nullptr)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    int run(HINSTANCE instance, int show)
    {
        int argc{};
        auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring capture;
        auto itemImagePath = ui::stimpakImagePath();
        auto inventoryPath = itemImagePath.parent_path() / "Inventory.json";
        bool editMounts=false, mirror=false, fullNotice=false, exercise=false, front=false, isometric=false, sample=false, stashPreview=false, selectionPreview=false;
        bool inventoryView=false, miscellaneous=false, overflow=false, heldWeapons=false, exerciseMisc=false;
        bool exerciseOverflow=false;
        bool pullPromptPreview=false;
        for (int i = 1; argv && i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--capture" && i + 1 < argc) capture = argv[++i];
            else if (std::wstring_view(argv[i]) == L"--item-preview" && i + 1 < argc) itemImagePath = argv[++i];
            else if (std::wstring_view(argv[i]) == L"--inventory" && i + 1 < argc) inventoryPath = argv[++i];
            else if (std::wstring_view(argv[i]) == L"--edit-mounts") editMounts=true;
            else if (std::wstring_view(argv[i]) == L"--right-hand-backpack") mirror=true;
            else if (std::wstring_view(argv[i]) == L"--full-notice") fullNotice=true;
            else if (std::wstring_view(argv[i]) == L"--exercise") exercise=true;
            else if (std::wstring_view(argv[i]) == L"--inventory-view") inventoryView=true;
            else if (std::wstring_view(argv[i]) == L"--misc") { inventoryView=true; miscellaneous=true; }
            else if (std::wstring_view(argv[i]) == L"--in-hand") { inventoryView=true; heldWeapons=true; }
            else if (std::wstring_view(argv[i]) == L"--overflow") { inventoryView=true; overflow=true; }
            else if (std::wstring_view(argv[i]) == L"--exercise-overflow") { inventoryView=true; overflow=true; exerciseOverflow=true; }
            else if (std::wstring_view(argv[i]) == L"--exercise-misc") { inventoryView=true; exerciseMisc=true; }
            else if (std::wstring_view(argv[i]) == L"--front") front=true;
            else if (std::wstring_view(argv[i]) == L"--isometric") isometric=true;
            else if (std::wstring_view(argv[i]) == L"--sample") sample=true;
            else if (std::wstring_view(argv[i]) == L"--pull-prompt-preview") pullPromptPreview=true;
            else if (std::wstring_view(argv[i]) == L"--stash-preview") stashPreview=true;
            else if (std::wstring_view(argv[i]) == L"--selection-preview") selectionPreview=true;
        }
        if (argv) LocalFree(argv);
        ImGui_ImplWin32_EnableDpiAwareness();
        WNDCLASSW wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = instance;
        wc.lpszClassName = L"ROCKBackpackPreview"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) return 1;
        constexpr auto style = WS_OVERLAPPEDWINDOW;
        RECT bounds{ 0, 0, 1440, 900 }; AdjustWindowRect(&bounds, style, FALSE);
        WindowState window;
        const auto hwnd = CreateWindowW(wc.lpszClassName,
            L"ROCK Backpack Preview | F11: fullscreen | F5: reload captured inventory", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, &window);
        if (!hwnd) return 1;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swap; ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11Texture2D> back;
        DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferCount = 2;
        desc.BufferDesc.Width = 1440; desc.BufferDesc.Height = 900;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd; desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};
        // Captures use a deterministic software D3D device; the interactive
        // preview uses the GPU, and both execute the same panel draw code.
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, capture.empty() ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
                nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, &level, &context)) ||
            FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) ||
            FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target))) return 2;
        ComPtr<IDXGIFactory> factory;
        if (SUCCEEDED(swap->GetParent(IID_PPV_ARGS(&factory)))) factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);
        ImGui::CreateContext(); ui::initializeStyle();
        if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX11_Init(device.Get(), context.Get())) return 3;
        preview::Simulation simulation(sample?sampleInventory():ui::loadPreviewInventory(inventoryPath).value_or(sampleInventory()));
        simulation.bagInRightHand=mirror; if(fullNotice) simulation.inventoryFullSeconds=2.2f;
        auto& model=simulation.model;
        if(exerciseOverflow) {
            model.items.clear(); model.grid=storage::Grid{};
            for(unsigned id:{9001u,9002u}) {
                model.grid.insert({id,storage::Kind::LongGun,1,0,{storage::Location::Overflow}});
                model.items.push_back({.id=std::to_string(id),.name=id==9001?"Overflow rifle to mount":"Overflow rifle to grid",.count=1,.formId=id,.storageId=id,.appearance=id});
            }
            simulation.changed();
        }
        const auto initialMiscCount=model.miscellaneous.size();
        if(stashPreview) { model.items.clear(); model.grid=storage::Grid{}; simulation.changed(); simulation.takeSample(storage::Kind::LongGun); }
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
        preview::OfflineScene scene;
        if(!scene.initialize(device.Get(),context.Get(),std::filesystem::path(executable).parent_path()/"PreviewAssets",1440,900)) return 6;
        std::string configError;
        if(const auto settings=rock_backpack::BackpackConfig::readOnly(configError); settings && !exercise) scene.configure(*settings);
        else model.status="Using compiled placement defaults: "+configError;
        const auto initialPlacement=scene.settings();
        const float initialMountX=scene.mountPosition(0).x;
        float editedWeaponX{};
        if(editMounts) scene.editMount(0);
        if(front) scene.front();
        if(isometric) scene.isometric();
        bool sceneMode=!inventoryView;
        ui::ItemImage itemImage;
        itemImage.load(device.Get(), itemImagePath, true);
        std::vector<ui::ItemPicture> pictures;
        const auto loadPictures = [&] {
            pictures.clear();
            for (const auto& item : model.items) {
                if (!item.appearance || std::any_of(pictures.begin(), pictures.end(), [&](const auto& picture) { return picture.appearance == item.appearance; })) continue;
                auto image = std::make_shared<ui::ItemImage>();
                const auto path = item.formId == ui::kStimpakForm ? itemImagePath : inventoryPath.parent_path() / (std::to_string(item.appearance) + ".png");
                if (image->load(device.Get(), path, true)) pictures.push_back({item.appearance, std::move(image)});
            }
        };
        loadPictures();
        ui::ViewState view; view.selected = "stimpak"; view.presentation = 1;
        view.miscellaneous=miscellaneous;
        view.overflow=overflow; view.heldWeapons=heldWeapons;
        if(miscellaneous || overflow || heldWeapons) view.selected.clear();
        if (capture.empty()) { ShowWindow(hwnd, show); UpdateWindow(hwnd); }
        bool done = false; unsigned frames{}, viewportWidth=1440, viewportHeight=900; int result = 0;
        while (!done) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
                if (message.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (capture.empty() && IsIconic(hwnd)) { WaitMessage(); continue; }
            // The hidden exercise covers swap-chain/depth resize and restores
            // the window before testing a world-panel click at its new pixels.
            if (exercise && (frames==1 || frames==3)) {
                window.toggle(hwnd);
                RECT checked{}; GetClientRect(hwnd,&checked);
                MONITORINFO monitor{sizeof(MONITORINFO)};
                const bool monitorValid=GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)!=0;
                const bool sizeValid=frames==1 ? monitorValid && checked.right==monitor.rcMonitor.right-monitor.rcMonitor.left &&
                    checked.bottom==monitor.rcMonitor.bottom-monitor.rcMonitor.top : checked.right==1440 && checked.bottom==900;
                if (!sizeValid || window.fullscreen!=(frames==1)) { result=8; break; }
            }
            RECT client{}; GetClientRect(hwnd,&client);
            const auto width=unsigned(client.right-client.left),height=unsigned(client.bottom-client.top);
            if (!width || !height) continue;
            if (width!=viewportWidth || height!=viewportHeight) {
                context->OMSetRenderTargets(0,nullptr,nullptr); target.Reset(); back.Reset();
                if (FAILED(swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0)) ||
                    FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))) ||
                    FAILED(device->CreateRenderTargetView(back.Get(),nullptr,&target)) || !scene.resize(device.Get(),width,height)) { result=8; break; }
                viewportWidth=width; viewportHeight=height;
            }
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
            if(exerciseOverflow && frames>=1 && frames<=20) {
                const bool second=frames>=10;
                const ImVec2 point=!second?(frames<4?ImVec2{520,138}:ImVec2{65,210}):
                    frames<13?ImVec2{520,138}:frames<16?ImVec2{430,80}:ImVec2{410,140};
                ImGui::GetIO().AddMousePosEvent(point.x,point.y);
                ImGui::GetIO().AddMouseButtonEvent(0,second?(frames>=11 && frames<19):(frames>=2 && frames<7));
            }
            if(exerciseMisc && frames>=1 && frames<=12) {
                const unsigned phase=(frames-1)%4;
                const ImVec2 point=frames<=4?ImVec2{610,80}:frames<=8?ImVec2{520,135}:ImVec2{65,630};
                ImGui::GetIO().AddMousePosEvent(point.x,point.y);
                ImGui::GetIO().AddMouseButtonEvent(0,phase==1 || phase==2);
            }
            if(exercise && frames>=5 && frames<10) {
                const auto point=scene.buttonScreen();
                ImGui::GetIO().AddMousePosEvent(point.x,point.y);
                ImGui::GetIO().AddMouseButtonEvent(0,frames==6 || frames==7);
            }
            if(exercise && frames==10) scene.editMount(0);
            if(exercise && frames>=11 && frames<20) {
                static float originalX{},originalY{};
                if(frames==11) { const auto p=scene.mountAxisScreen(); originalX=p.x; originalY=p.y; }
                ImGui::GetIO().AddMousePosEvent(originalX+(frames>=14?-24.f:0.f),originalY);
                ImGui::GetIO().AddMouseButtonEvent(0,frames>=12 && frames<16);
            }
            if(exercise && frames==20) { editedWeaponX=scene.mountPosition(0).x; scene.editStash(1,false); }
            if(exercise && frames==30) scene.editStash(1,true);
            if(exercise && frames>=21 && frames<40) {
                static float startX{},startY{};
                const unsigned phase=frames%10;
                if(phase==1) {
                    const auto& placement=scene.settings();
                    const auto p=scene.project(placement.stash.centers[1]+rock_backpack::panel_math::Vec3{frames<30?8.f:placement.stash.halves[1].x*.85f,0,0});
                    startX=p.x; startY=p.y;
                }
                ImGui::GetIO().AddMousePosEvent(startX+(phase>=4?-20.f:0.f),startY);
                ImGui::GetIO().AddMouseButtonEvent(0,phase>=2 && phase<6);
            }
            if(exercise && frames==40) scene.editMount(0,true);
            if(exercise && frames>=41) {
                const float angle=(frames>=44?80.f:35.f)*.01745329252f;
                const auto p=scene.project(scene.mountPosition(0)+rock_backpack::panel_math::Vec3{9.f*std::cos(angle),9.f*std::sin(angle),0});
                ImGui::GetIO().AddMousePosEvent(p.x,p.y);
                ImGui::GetIO().AddMouseButtonEvent(0,frames>=42 && frames<46);
            }
            if((stashPreview || selectionPreview) && frames>=1) {
                const auto point=scene.depositScreen();
                ImGui::GetIO().AddMousePosEvent(point.x,point.y);
            }
            ImGui::NewFrame();
            if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
                model = ui::loadPreviewInventory(inventoryPath).value_or(sampleInventory()); view = {}; view.selected = "stimpak"; view.presentation = 1;
                itemImage = {}; itemImage.load(device.Get(), itemImagePath, true); loadPictures();
            }
            const auto action = sceneMode || pullPromptPreview ? std::optional<ui::Action>{} : ui::drawPanel(model, view, {}, ui::Hand::Right, &itemImage,
                itemImage.view ? "Captured Stimpak model" : "No captured model yet", pictures);
            auto* rtv = target.Get(); context->OMSetRenderTargets(1, &rtv, nullptr);
            const float clear[]{ .055f, .075f, .062f, 1 }; context->ClearRenderTargetView(rtv, clear);
            if(ImGui::IsKeyPressed(ImGuiKey_F3)) sceneMode=!sceneMode;
            if(pullPromptPreview) {
                ui::PullPrompt prompt;
                prompt.action=ui::PullPrompt::Action::GripBag;
                ui::drawPullPrompt(prompt,{215,340});
                prompt.action=ui::PullPrompt::Action::PullBag;
                ui::drawPullPrompt(prompt,{565,340});
                prompt.action=ui::PullPrompt::Action::RearmBag;
                ui::drawPullPrompt(prompt,{915,340});
            } else if(sceneMode) scene.frame(context.Get(),target.Get(),simulation,pictures,&itemImage);
            ImGui::Render();
            context->OMSetRenderTargets(1,&rtv,nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (action) simulation.apply(*action);
            if (!capture.empty() && ++frames >= (exercise?50u:exerciseOverflow?24u:exerciseMisc?20u:4u)) {
                const bool miscFailed=exerciseMisc && (!view.miscellaneous || !simulation.hands[0] ||
                    simulation.hands[0]->kind!=storage::Kind::Misc || model.miscellaneous.size()+1!=initialMiscCount);
                const bool overflowFailed=exerciseOverflow && (!model.grid.find(9001) || !model.grid.find(9002) ||
                    model.grid.find(9001)->placement.location!=storage::Location::LeftRifle ||
                    model.grid.find(9002)->placement.location!=storage::Location::Interior || model.items.size()!=2);
                result = overflowFailed || miscFailed || (exercise && (!simulation.open || scene.mountPosition(0).x<=initialMountX ||
                    !simulation.model.grid.at(rock_backpack::storage::Location::LeftRifle) ||
                    scene.settings().stash.centers[1].x<=initialPlacement.stash.centers[1].x ||
                    scene.settings().stash.halves[1].x<=initialPlacement.stash.halves[1].x ||
                    scene.settings().mounts.rotations[0].z<30.f || scene.mountPosition(0).x!=editedWeaponX)) ? 7 : saveCapture(device.Get(), context.Get(), back.Get(), capture.c_str()) ? 0 : 5;
                break;
            }
            swap->Present(capture.empty() ? 1 : 0, 0);
        }
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, instance);
        return result;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try { result = run(instance, show); }
    catch (...) { OutputDebugStringW(L"ROCK Backpack preview failed.\n"); }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
