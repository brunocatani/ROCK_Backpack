#include "PCH.h"
#include "NativeItemPreview.h"
#include "BackpackScene.h"
#include "NativePreviewAddresses.h"
#include "NativePreviewCaptureThunk.h"
#include "NativePreviewLighting.h"
#include "WeaponMaterialFix.h"
#include "PreviewRecovery.h"
#include "ui/PreviewAlphaPass.h"
#include <F4SE/Trampoline.h>
#include <RPS/Runtime/HookChain.h>
#include <cstring>
#include <RE/Bethesda/TESBoundObjects.h>

namespace rock_backpack::item_preview
{
    namespace
    {
        namespace native = native_preview;
        using Microsoft::WRL::ComPtr;
        template<class F> F entry(const native::Entry& value)
        { return reinterpret_cast<F>(REL::Module::get().base() + value.rva); }
        // What each patched site reached: the native function or another plugin's
        // detour. Written at install before the site is patched, then read-only.
        std::uintptr_t dispatchCallee{}, sceneUpdateCallee{}, postForwardCallee{}, releaseCallee{};
        template<class T> T& field(void* object, std::size_t offset)
        { return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset); }

        enum class Status { Unavailable, Waiting, Loading, Capturing, Ready, Failed, Unsupported };
        struct Cached
        {
            Request request;
            Status status{Status::Waiting};
            preview_recovery::Retry retry;
            std::shared_ptr<const ui::ItemImage> image;
            RE::NiPointer<RE::NiAVObject> visual;
        };
        struct State
        {
            std::atomic_bool installed{}, ready{}, visible{};
            std::atomic_bool sceneRequested{};
            std::atomic_uint64_t epoch{ 1 };
            std::atomic<Status> status{ Status::Unavailable };
            // Everything below is native-dispatch-thread owned, except the
            // immutable image publication guarded by mailbox. RPS never sees a
            // native node, reference, target number, or pooled engine texture.
            std::atomic<DWORD> thread{}, uiThread{};
            std::atomic_bool threadMismatchReported{};
            std::uint64_t activeEpoch{}, started{}, observedEpoch{};
            bool attempted{}, managerConstructed{}, itemBegun{}, captured{}, exporting{};
            unsigned readbackFrames{};
            unsigned captureVisits{};
            unsigned lightingPasses{};
            const char* captureStage{ "model load" };
            native::LightingScope lighting;
            weapon_material_fix::Preparation materials;
            bool weaponPreview{}, materialsReady{};
            ui::PreviewAlphaPass alpha;
            void* renderer{};
            alignas(16) std::array<std::byte, 0x150> manager{};
            ComPtr<ID3D11DeviceContext> context;
            ui::ImageReadback readback;
            std::shared_ptr<const ui::ItemImage> pendingImage;
            std::mutex mailbox;
            std::shared_ptr<const ui::ItemImage> published;
            std::uint64_t publishedEpoch{}, uiDeviceGeneration{};
            ComPtr<ID3D11Device> uiDevice;
            Request active;
            std::vector<Cached> cache; // mailbox protects requests and completed images
            std::shared_ptr<const std::vector<ui::ItemPicture>> pictures;
        };
        State& state() { static State value; return value; }

        bool placementReady(const State& s)
        {
            // Native tween +0x44 (1312FD0/13120A0), read as record +0x84
            // by 13067A0. Exactly one item was requested; no stride is assumed.
            const auto* record = *reinterpret_cast<const std::byte* const*>(s.manager.data() + 0x30);
            return record && *reinterpret_cast<const std::uint32_t*>(s.manager.data() + 0x40) == 1 && record[0x84] != std::byte{};
        }

        void captureForwardImage(State& s, ID3D11DeviceContext* context)
        {
            if (s.captured || !s.ready.load() || !s.visible.load() || s.activeEpoch != s.epoch.load()) return;
            ++s.captureVisits; s.captureStage = "native placement";
            if (!placementReady(s)) return;
            if (!s.materialsReady) { s.captureStage = "weapon material culling"; return; }
            s.captureStage = "forward texture copy";
            auto image = std::make_shared<ui::ItemImage>();
            // The 2026-09-27 runtime probe found all three Stimpak shapes here,
            // but zero pixels in the same texture at native target release.
            // Own the pixels after lighting/alpha, before native composition.
            if (!image->copyRenderTarget(context) || !s.readback.begin(context, *image)) {
                s.status = Status::Failed;
                logger::error("Item preview could not copy/read back its forward render target.");
                return;
            }
            s.pendingImage = image; s.context = context; s.readbackFrames = 0;
            s.captured = s.exporting = true; s.status = Status::Capturing;
            D3D11_TEXTURE2D_DESC desc{}; image->texture->GetDesc(&desc);
            logger::info("Item preview copied before native composition; awaiting pixel validation: epoch={}, pass={}, {}x{}, format={}, captureThread={}, rpsThread={}.",
                s.activeEpoch, s.lightingPasses, image->width, image->height, unsigned(desc.Format), s.thread.load(), s.uiThread.load());
        }

        void closeNative(State& s)
        {
            s.lighting.restore();
            s.alpha.clearDevice();
            if (s.renderer) entry<void(*)(void*)>(native::disableRenderer)(s.renderer);
            if (s.itemBegun) {
                entry<void(*)(void*)>(native::endItem)(s.manager.data());
                s.itemBegun = false;
            }
            if (s.managerConstructed) {
                entry<void(*)(void*)>(native::destroyManager)(s.manager.data());
                s.managerConstructed = false;
            }
            if (s.renderer) {
                entry<void(*)(void*)>(native::releaseRenderer)(s.renderer);
                s.renderer = nullptr;
            }
        }

        void beginNative(State& s)
        {
            s.attempted = true; s.captured = false; s.started = GetTickCount64();
            s.activeEpoch = s.epoch.load(std::memory_order_acquire);
            s.status = Status::Loading;
            s.captureVisits = s.lightingPasses = 0; s.captureStage = "model load";
            s.materials = {};
            logger::info("Item preview initializing native scene: form={:08X}, appearance={}, epoch={}, thread={}.", s.active.form, s.active.appearance, s.activeEpoch, s.thread.load());
            auto* form = RE::TESForm::GetFormByID(s.active.form);
            if (!form || !form->As<RE::TESBoundObject>() || !RE::PlayerCharacter::GetSingleton()) {
                s.status = Status::Failed;
                logger::error("Item preview: form {:08X} or player unavailable.", s.active.form);
                return;
            }
            if(const auto* misc=form->As<RE::TESObjectMISC>();misc && (!misc->GetModel() || !*misc->GetModel())) {
                s.status=Status::Unsupported;
                logger::info("Item has no preview model; retain its named inventory row: form={:08X}",s.active.form); return;
            }
            s.weaponPreview = form->Is(RE::ENUM_FORM_ID::kWEAP);
            s.materialsReady = !s.weaponPreview;
            RE::BSFixedString name("ROCKBackpack.ItemPreview");
            if (entry<void*(*)(const RE::BSFixedString*)>(native::findRenderer)(&name)) {
                s.status = Status::Failed;
                logger::error("Item preview: renderer name already owned; refusing to reuse it.");
                return;
            }
            s.renderer = entry<void*(*)(const RE::BSFixedString*, std::uint32_t, float, bool)>(native::createRenderer)(&name, 0, 0.f, true);
            if (!s.renderer) { s.status = Status::Failed; return; }
            entry<void(*)(void*, std::uint32_t)>(native::setSize)(s.renderer, 1);
            // No screen-attached geometry: the native main-screen pass returns
            // without drawing. Only the private offscreen image is consumed.
            entry<void(*)(void*, bool)>(native::enableRenderer)(s.renderer, false);
            if (field<std::int32_t>(s.renderer, 0x1D8) < 0 || field<std::int32_t>(s.renderer, 0x1DC) < 0) {
                closeNative(s); s.status = Status::Failed;
                logger::error("Item preview: native custom target pool exhausted.");
                return;
            }
            s.manager.fill(std::byte{});
            // 0x150 is the aligned native subobject extent in ContainerMenuBase
            // (0x2B0..0x400) and the workbench constructor (0x1C0..0x310).
            // The inherited CommonLib 0x140 declaration is not used.
            entry<void*(*)(void*)>(native::constructManager)(s.manager.data());
            s.managerConstructed = true;
            // Constructor +0xC0, native lookup/install, and menu caller agree.
            field<RE::BSFixedString>(s.manager.data(), 0xC0) = name;
            entry<void(*)(void*)>(native::beginItem)(s.manager.data());
            s.itemBegun = true;
            // 13072B0 calls this resolver before creating its task. A null
            // result creates no task, so waiting fifteen seconds cannot help.
            if(!entry<RE::TESForm*(*)(void*,RE::TESForm*)>(native::resolveObject)(s.manager.data(),form)) {
                closeNative(s); s.status=Status::Unsupported;
                logger::info("Native item has no inventory presentation: form={:08X}",s.active.form); return;
            }
            const float center[]{ .5f, .5f, 1.f };
            entry<void(*)(void*, const float*, bool)>(native::setPosition)(s.manager.data(), center, true);
            // Native async preview task builds its own reference/model. No
            // world item is spawned, equipped, removed or taken from a hand.
            // 14130738D..3B6 copies caller extra data into the preview's own
            // reference before queuing the native task. Retain the input through
            // that copy; never borrow a mutable player inventory pointer.
            entry<void(*)(void*, RE::TESForm*, RE::ExtraDataList*, std::uint32_t)>(native::loadItem)(s.manager.data(), form, s.active.extra.get(), 1);
            logger::info("Item preview: async native request, epoch={}, dispatchThread={}.", s.activeEpoch, s.thread.load());
        }

        void service(State& s)
        {
            const auto current = GetCurrentThreadId();
            if (!s.thread.load()) {
                if (!s.ready.load() || (!s.visible.load() && !s.sceneRequested.load())) return;
                DWORD expected{};
                s.thread.compare_exchange_strong(expected, current);
            }
            if (s.thread.load() != current) {
                if (!s.threadMismatchReported.exchange(true))
                    logger::error("Item preview: dispatch thread changed from {} to {}; skipping native work on the unexpected thread.", s.thread.load(), current);
                return;
            }
            const auto epoch = s.epoch.load(std::memory_order_acquire);
            if (s.observedEpoch != epoch) {
                closeNative(s); s.readback.clear(); s.context.Reset(); s.pendingImage.reset(); s.exporting = false;
                std::unique_lock lock(s.mailbox, std::try_to_lock);
                if (!lock) return;
                s.observedEpoch = epoch; s.attempted = false;
                {
                    s.published.reset(); s.publishedEpoch = 0; s.pictures.reset();
                    for (auto& cached : s.cache) { cached.status = Status::Waiting; cached.retry={}; cached.image.reset(); }
                }
                s.active = {};
                s.status = Status::Waiting;
            }
            const bool active = s.ready.load(std::memory_order_acquire) && (s.visible.load(std::memory_order_acquire) || s.sceneRequested.load(std::memory_order_acquire));
            if (!active) {
                if (s.renderer) closeNative(s);
                if (s.attempted && !s.exporting && (!s.captured || s.status != Status::Capturing)) {
                    std::unique_lock lock(s.mailbox, std::try_to_lock);
                    if (lock) {
                        for (auto& cached : s.cache) if (cached.request.appearance == s.active.appearance && cached.status == Status::Loading) cached.status = Status::Waiting;
                        s.active = {}; s.attempted = false;
                    }
                }
            }
            if (s.attempted && !s.renderer && !s.exporting && (s.status == Status::Ready || s.status == Status::Failed || s.status==Status::Unsupported)) {
                std::unique_lock lock(s.mailbox, std::try_to_lock);
                if (lock) {
                    for (auto& cached : s.cache) if (cached.request.appearance == s.active.appearance) {
                        if(s.status==Status::Unsupported) cached.status=Status::Unsupported;
                        else if(s.status==Status::Failed) {
                            cached.status=Status::Failed; cached.retry.failed(GetTickCount64());
                            logger::warn("Item preview retry deferred: form={:08X}, appearance={}, visual={}, image={}, delayMs={}",
                                cached.request.form,cached.request.appearance,bool(cached.visual),bool(cached.image),cached.retry.after-GetTickCount64());
                        } else { cached.status=Status::Ready; cached.retry={}; }
                    }
                    s.active = {}; s.attempted = false;
                }
            }
            if(active && s.renderer && !s.captured && !s.exporting) {
                std::unique_lock lock(s.mailbox,std::try_to_lock);
                if(lock) {
                    const auto current=std::find_if(s.cache.begin(),s.cache.end(),[&](const auto& c){return c.request.appearance==s.active.appearance;});
                    const bool buildingMount=current!=s.cache.end() && current->request.scene && !current->visual;
                    const bool urgent=std::any_of(s.cache.begin(),s.cache.end(),[&](const auto& c){return c.request.scene && !c.visual && c.request.appearance!=s.active.appearance && c.status!=Status::Unsupported && c.retry.due(GetTickCount64());});
                    if(!buildingMount && urgent) {
                        if(current!=s.cache.end()) current->status=Status::Waiting;
                        lock.unlock();
                        // Same owned cancellation path used for visibility and
                        // session changes, before native renderer iteration.
                        closeNative(s); s.active={}; s.attempted=false; s.status=Status::Waiting;
                    }
                }
            }
            if (active && !s.attempted) {
                std::unique_lock lock(s.mailbox, std::try_to_lock);
                if (lock) {
                    const auto now=GetTickCount64();
                    // Missing body/mount geometry precedes optional thumbnails.
                    // A stable inventory does not need to republish requests to
                    // recover an earlier transient failure or changed demand.
                    for(unsigned pass=0;pass<2 && !s.active.appearance;++pass)
                        for(auto& cached:s.cache) {
                            const bool missingScene=cached.request.scene && !cached.visual;
                            if(missingScene!=(pass==0) || cached.status==Status::Loading || cached.status==Status::Unsupported || !cached.retry.due(now) ||
                                !preview_recovery::needed(cached.request.scene,s.visible.load(),bool(cached.visual),bool(cached.image))) continue;
                            s.active=cached.request; cached.status=Status::Loading; break;
                        }
                    lock.unlock();
                    if (s.active.appearance) beginNative(s);
                }
            }
            if (active && s.renderer && !s.captured) {
                if (GetTickCount64() - s.started > 15000) {
                    logger::error("Item preview timed out: stage={}, model={}, items={}, captureVisits={}.",
                        s.captureStage, field<void*>(s.renderer, 0x98) != nullptr,
                        field<std::uint32_t>(s.manager.data(), 0x40), s.captureVisits);
                    closeNative(s); s.status = Status::Failed;
                } else {
                    {
                        std::unique_lock lock(s.mailbox,std::try_to_lock);
                        if(lock) for(const auto& cached:s.cache) if(cached.request.appearance==s.active.appearance) {
                            s.active.scene=cached.request.scene; break;
                        }
                    }
                    entry<void(*)(void*)>(native::updateItem)(s.manager.data());
                    if(s.active.scene) if(auto* root=field<RE::NiAVObject*>(s.renderer,0x98)) {
                        // The native loader owns the source. Publish only a
                        // detached collisionless clone; no world item is made.
                        bool published{};
                        {
                            std::unique_lock lock(s.mailbox,std::try_to_lock);
                            if(lock) for(auto& cached:s.cache) if(cached.request.appearance==s.active.appearance) {
                                published=bool(cached.visual);
                                if(published && !s.visible.load()) cached.status=Status::Ready;
                                break;
                            }
                        }
                        if(published && !s.visible.load()) {
                            s.captured=true; closeNative(s); s.status=Status::Ready; return;
                        }
                    }
                    if (field<void*>(s.renderer, 0x98) && s.status != Status::Capturing) {
                        s.status = Status::Capturing;
                        logger::info("Item preview native model loaded; waiting for placement and capture.");
                    }
                }
            }
            if (s.exporting) {
                ui::ImagePixels pixels;
                const auto result = s.readback.poll(s.context.Get(), pixels);
                if (result == ui::ReadbackResult::Pending && ++s.readbackFrames < 120) return;
                s.readback.clear(); s.context.Reset(); s.exporting = false;
                if (result != ui::ReadbackResult::Ready) {
                    s.pendingImage.reset(); s.status = Status::Failed;
                    logger::warn("Item preview: nonblocking PNG readback failed or expired.");
                    return;
                }
                if (const auto* tasks = F4SE::GetTaskInterface()) tasks->AddTask([
                    pixels = std::move(pixels), image = std::move(s.pendingImage), epoch = s.activeEpoch, passes = s.lightingPasses,
                    key = s.active.appearance, form = s.active.form] {
                    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    struct ComScope { bool active; ~ComScope() { if (active) CoUninitialize(); } } comScope{SUCCEEDED(com)};
                    try {
                        const auto coverage = ui::inspectItemImage(pixels);
                        auto& current = state();
                        const auto thumbnail = ui::itemThumbnail(pixels);
                        ComPtr<ID3D11Device> device;
                        if (image && image->texture) image->texture->GetDevice(&device);
                        auto owned = std::make_shared<ui::ItemImage>();
                        const bool uploaded = owned->upload(device.Get(), thumbnail);
                        {
                            std::scoped_lock lock(current.mailbox);
                            if (current.epoch.load() != epoch || !current.ready.load()) {
                                return;
                            }
                            if (!uploaded || !coverage.valid || !coverage.alphaPixels ||
                                (pixels.format == DXGI_FORMAT_R11G11B10_FLOAT && !coverage.colorPixels)) {
                                current.status = Status::Failed;
                                logger::error("Item preview rejected empty capture: colorPixels={}, alphaPixels={}, lightingPasses={}, epoch={}.",
                                    coverage.colorPixels, coverage.alphaPixels, passes, epoch);
                                return;
                            }
                            for (auto& cached : current.cache) if (cached.request.appearance == key) {
                                cached.image = owned; cached.status = Status::Ready;
                            }
                            auto pictures = std::make_shared<std::vector<ui::ItemPicture>>();
                            for (const auto& cached : current.cache) if (cached.image) pictures->push_back({cached.request.appearance, cached.image});
                            current.pictures = std::move(pictures);
                            if (form == ui::kStimpakForm) current.published = owned;
                            current.publishedEpoch = epoch; current.status = Status::Ready;
                        }
                        logger::info("Item preview validated: colorPixels={}, alphaPixels={}, lightingPasses={}, epoch={}.",
                            coverage.colorPixels, coverage.alphaPixels, passes, epoch);
                        const auto path = form == ui::kStimpakForm ? ui::stimpakImagePath() :
                            ui::stimpakImagePath().parent_path() / (std::to_string(key) + ".png");
                        if (ui::saveItemImage(thumbnail, path)) logger::info("Item preview exported for desktop: {} (form={:08X}, appearance={}).", path.string(), form, key);
                        else logger::warn("Item preview PNG export failed.");
                    } catch (...) {
                        auto& current = state();
                        if (current.epoch.load() == epoch) current.status = Status::Failed;
                        logger::error("Item preview validation/export failed with an exception.");
                    }
                }); else { s.pendingImage.reset(); s.status = Status::Failed; }
            }
        }

        void dispatchHook(std::uint32_t target, bool phase) noexcept
        {
            auto& s = state();
            try { if (s.installed.load(std::memory_order_acquire)) service(s); }
            catch (...) { s.status = Status::Failed; s.attempted = true; logger::error("Item preview setup failed."); }
            reinterpret_cast<void(*)(std::uint32_t, bool)>(dispatchCallee)(target, phase);
            // The registry is no longer being iterated/locked here. Never
            // remove an owned renderer inside its draw or target-release hook.
            if (s.thread.load() == GetCurrentThreadId() && (s.captured || s.status == Status::Failed)) closeNative(s);
        }

        void sceneUpdateHook(void* scene, void* update) noexcept
        {
            reinterpret_cast<void(*)(void*, void*)>(sceneUpdateCallee)(scene, update);
            auto& s = state();
            if (s.thread.load() != GetCurrentThreadId() || !s.renderer || s.status == Status::Failed ||
                scene != field<void*>(s.renderer, 0x258)) return;
            if (s.weaponPreview)
                s.materialsReady = s.materials.update(field<RE::NiAVObject*>(s.renderer, 0x98), s.active.form);
            if(s.active.scene && s.materialsReady) {
                auto* root=field<RE::NiAVObject*>(s.renderer,0x98);
                // Run after native scene assembly/material preparation and
                // before preview lighting temporarily mutates shader flags.
                std::unique_lock lock(s.mailbox,std::try_to_lock);
                if(lock && root) for(auto& cached:s.cache) if(cached.request.appearance==s.active.appearance) {
                    if(!cached.visual) cached.visual=BackpackScene::cloneVisual(root);
                    break;
                }
            }
            // Physical geometry does not require the offscreen thumbnail pass.
            // Let native material preparation finish before applying lighting;
            // an early/pending model must not become a terminal cache failure.
            if(!s.visible.load() || !s.materialsReady) return;
            if (!s.lighting.begin(s.renderer)) {
                s.status = Status::Failed;
                logger::error("Item preview could not prepare the isolated lighting pass.");
            } else if (!s.lightingPasses) {
                logger::info("Item preview lighting prepared: geometries={}, preview-only environment flags={}.",
                    s.lighting.geometryCount(), s.lighting.environmentCount());
            }
        }

        void postForwardHook(void* accumulator, void* renderContext) noexcept
        {
            auto& s = state();
            if (s.thread.load() == GetCurrentThreadId() && s.renderer && s.lighting.owns(accumulator) && s.status != Status::Failed) {
                try {
                    auto* manager = reinterpret_cast<void*>(REL::Module::get().base() + 0x38AC010);
                    const auto target = field<std::uint32_t>(s.renderer, 0x1D8);
                    const auto physical = entry<std::int32_t(*)(void*, std::uint32_t)>(native::resolveTarget)(manager, target);
                    auto* graphics = reinterpret_cast<void*>(REL::Module::get().base() + 0x6239340);
                    auto* texture = physical < 0 ? nullptr : entry<ID3D11Texture2D*(*)(void*, std::uint32_t)>(native::getTexture)(graphics, physical);
                    ComPtr<ID3D11DeviceContext> context;
                    if (texture) {
                        ComPtr<ID3D11Device> device;
                        texture->GetDevice(&device); device->GetImmediateContext(&context);
                    }
                    // All Stimpak geometry is in these lighting groups. Keep
                    // native finalization and the later composition untouched.
                    for (std::uint8_t group = 0; group != 3; ++group)
                        entry<void(*)(void*, std::uint8_t, void*, bool)>(native::shaderGroup)(accumulator, group, renderContext, false);
                    ++s.lightingPasses;
                    if (context && s.alpha.apply(context.Get())) captureForwardImage(s, context.Get());
                    else { s.status = Status::Failed; logger::error("Item preview forward-target alpha pass failed."); }
                } catch (...) {
                    s.status = Status::Failed;
                    logger::error("Item preview forward pass failed with an exception.");
                }
            }
            reinterpret_cast<void(*)(void*, void*)>(postForwardCallee)(accumulator, renderContext);
        }

        void releaseHook(void* manager, std::uint32_t target, void* rendering) noexcept
        {
            auto& s = state();
            // The image is already owned. Retain this boundary solely to
            // restore preview material/accumulator state after the native pass.
            if (s.thread.load() == GetCurrentThreadId() && rendering == s.renderer) s.lighting.restore();
            reinterpret_cast<void(*)(void*, std::uint32_t)>(releaseCallee)(manager, target);
        }
    }

    bool install()
    {
        auto& s = state();
        const auto base = REL::Module::get().base();
        const auto matches = [base](const auto& guard) { return std::memcmp(reinterpret_cast<const void*>(base + guard.rva), guard.bytes.data(), guard.bytes.size()) == 0; };
        // Called functions: their prologue, or another plugin's detour at the entry.
        for (const auto& guard : native::entries) if (!RPS::Runtime::HookChain::resolveEntry(base + guard.rva, guard.bytes)) {
            logger::error("Item preview disabled: native function guard mismatch at RVA {:X}.", guard.rva); return false;
        }
        if (!matches(native::offscreenOwner)) {
            logger::error("Item preview disabled: native renderer register setup changed."); return false;
        }
        // Hook sites: the native call or jump, or another plugin's detour there,
        // which the hooks then call through.
        const auto dispatch = RPS::Runtime::HookChain::resolveEntry(base + native::dispatchJump.rva, native::dispatchJump.bytes);
        const auto release = RPS::Runtime::HookChain::resolveCall(base + native::releaseCall.rva, base + native::releaseTarget.rva);
        const auto sceneUpdate = RPS::Runtime::HookChain::resolveCall(base + native::sceneUpdateCall.rva, base + native::sceneUpdate.rva);
        const auto postForward = RPS::Runtime::HookChain::resolveCall(base + native::postForwardCall.rva, base + native::postForward.rva);
        const std::array sites{ &native::dispatchJump, &native::releaseCall, &native::sceneUpdateCall, &native::postForwardCall };
        const std::array resolved{ &dispatch, &release, &sceneUpdate, &postForward };
        for (std::size_t index = 0; index < sites.size(); ++index) {
            if (!*resolved[index]) {
                const auto* actual = reinterpret_cast<const std::uint8_t*>(base + sites[index]->rva);
                logger::error("Item preview disabled: hook-site mismatch at RVA {:X}; actual {:02X} {:02X} {:02X} {:02X} {:02X}.",
                    sites[index]->rva, actual[0], actual[1], actual[2], actual[3], actual[4]); return false;
            }
            if (resolved[index]->foreign())
                logger::info("Item preview: RVA {:X} is already hooked by {}; Backpack calls through it", sites[index]->rva, resolved[index]->owner);
        }
        std::array<std::array<std::uint8_t, 5>, 4> before{};
        for (std::size_t index = 0; index < sites.size(); ++index)
            std::memcpy(before[index].data(), reinterpret_cast<const void*>(base + sites[index]->rva), 5);
        // A native tail jump continues into the dispatcher; a foreign one into its detour.
        dispatchCallee = dispatch.foreign() ? dispatch.callee : base + native::dispatch.rva;
        releaseCallee = release.callee;
        sceneUpdateCallee = sceneUpdate.callee;
        postForwardCallee = postForward.callee;
        if (!s.alpha.compile()) { logger::error("Item preview alpha shaders could not compile."); return false; }
        // All sites and every called native entry are checked before the
        // first write. Hooks are process-lifetime; published engine APIs stay intact.
        unsigned written{};
        try {
            F4SE::AllocTrampoline(256);
            auto& trampoline = F4SE::GetTrampoline();
            const native::CaptureThunk capture(reinterpret_cast<std::uintptr_t>(&releaseHook));
            auto* code = trampoline.allocate(capture);
            written = 1; trampoline.write_call<5>(base + native::releaseCall.rva, reinterpret_cast<std::uintptr_t>(code));
            written = 2; trampoline.write_call<5>(base + native::sceneUpdateCall.rva, sceneUpdateHook);
            written = 3; trampoline.write_call<5>(base + native::postForwardCall.rva, postForwardHook);
            written = 4; trampoline.write_branch<5>(base + native::dispatchJump.rva, dispatchHook);
        } catch (...) {
            // Write order: release, scene update, post forward, dispatch.
            constexpr std::array order{ 1, 2, 3, 0 };
            while (written) {
                const auto index = order[--written];
                REL::safe_write(base + sites[index]->rva, before[index].data(), before[index].size());
            }
            logger::error("Item preview hook installation failed; original instructions restored.");
            return false;
        }
        s.installed.store(true, std::memory_order_release); s.status = Status::Waiting;
        logger::info("Item preview installed with owned lighting/alpha and validated capture; Inventory3DFix hooks left intact (loaded={}).",
            GetModuleHandleW(L"Inventory3DFix.dll") != nullptr);
        return true;
    }
    void setSessionReady(bool ready) noexcept
    {
        auto& s = state(); s.ready.store(ready, std::memory_order_release);
        std::scoped_lock lock(s.mailbox);
        s.cache.clear(); s.pictures.reset(); s.published.reset(); s.sceneRequested=false;
        s.epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    void setVisible(bool visible) noexcept { state().visible.store(visible, std::memory_order_release); }

    void request(std::span<const Request> items)
    {
        auto& s = state();
        std::scoped_lock lock(s.mailbox);
        std::vector<Cached> next; next.reserve((std::min)(items.size(), std::size_t(128)));
        for (const auto& item : items) {
            if (!item.appearance || !item.form || next.size() == 128) continue;
            if (std::any_of(next.begin(), next.end(), [&](const auto& cached) { return cached.request.appearance == item.appearance; })) continue;
            const auto found = std::find_if(s.cache.begin(), s.cache.end(), [&](const auto& cached) { return cached.request.appearance == item.appearance; });
            if (found == s.cache.end()) next.push_back({item});
            else {
                next.push_back(*found); next.back().request = item;
                if(((s.visible.load() && !next.back().image) || (item.scene && !next.back().visual)) && next.back().status==Status::Ready) next.back().status=Status::Waiting;
            }
        }
        s.cache = std::move(next);
        s.sceneRequested=std::any_of(s.cache.begin(),s.cache.end(),[](const auto& item){return item.request.scene;});
        auto pictures = std::make_shared<std::vector<ui::ItemPicture>>();
        for (const auto& cached : s.cache) if (cached.image) pictures->push_back({cached.request.appearance, cached.image});
        s.pictures = std::move(pictures);
    }

    void publishScene(std::uint64_t appearance,std::uint32_t form,RE::NiPointer<RE::NiAVObject> visual)
    {
        if(!appearance || !form || !visual || visual->parent) return;
        auto& s=state(); std::scoped_lock lock(s.mailbox);
        if(!s.ready.load()) return;
        auto found=std::find_if(s.cache.begin(),s.cache.end(),[&](const auto& c){return c.request.appearance==appearance;});
        if(found==s.cache.end()) {
            if(s.cache.size()==128) {
                const auto spare=std::find_if(s.cache.rbegin(),s.cache.rend(),[](const auto& c){return !c.request.scene && c.status!=Status::Loading;});
                if(spare==s.cache.rend()) return;
                s.cache.erase(std::next(spare).base());
            }
            s.cache.push_back({Request{appearance,form,{},true}}); found=std::prev(s.cache.end());
        }
        found->visual=std::move(visual); found->request.scene=true; s.sceneRequested=true;
        logger::info("Physical weapon display preserved from world item: form={:08X}, appearance={}",form,appearance);
    }
    RE::NiPointer<RE::NiAVObject> scene(std::uint64_t appearance)
    {
        auto& s=state(); std::unique_lock lock(s.mailbox,std::try_to_lock);
        if(!lock || !s.ready.load()) return {};
        for(const auto& cached:s.cache) if(cached.request.appearance==appearance) return cached.visual;
        return {};
    }

    Snapshot snapshot(ID3D11Device* device, std::uint64_t deviceGeneration)
    {
        auto& s = state();
        s.uiThread.store(GetCurrentThreadId());
        std::unique_lock lock(s.mailbox, std::try_to_lock);
        if (!lock) return { {}, "Preparing item previews..." };
        if (s.uiDevice && (s.uiDevice.Get() != device || s.uiDeviceGeneration != deviceGeneration)) {
            s.epoch.fetch_add(1, std::memory_order_acq_rel);
            s.published.reset();
        }
        s.uiDevice = device; s.uiDeviceGeneration = deviceGeneration;
        if (s.ready.load() && s.publishedEpoch == s.epoch.load() && s.published && s.published->belongsTo(device))
            return { s.published, "Native item models", s.pictures };
        if (s.ready.load() && s.publishedEpoch == s.epoch.load() && s.pictures && !s.pictures->empty())
            return { {}, "Native item models", s.pictures };
        switch (s.status.load()) {
        case Status::Unavailable: return { {}, "Item preview unavailable; check ROCKBackpack.log." };
        case Status::Failed: return { {}, "Item preview failed; check ROCKBackpack.log." };
        case Status::Loading: return { {}, "Loading item model..." };
        case Status::Capturing: return { {}, "Rendering item model..." };
        default: return { {}, "Preparing item previews..." };
        }
    }
}
