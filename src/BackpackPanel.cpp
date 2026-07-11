#include "PCH.h"
#include "BackpackPanel.h"
#include "ui/PanelView.h"
#include "ui/PointerPressGate.h"
#include "NativeItemPreview.h"
#include "ui/ZoneGuidance.h"
#include <RPS_UI_Framework/Client.h>
#include <RPS_UI_Framework/PassivePanel.h>
#include <imgui_internal.h>
#include <RPS_UI_Framework/ImGuiBuild.h>

namespace rock_backpack
{
    // Render thread only. RPS UI owns the panel's ImGui context; the style and
    // font set up for it in setup() live with that context generation.
    struct BackpackPanel::RenderState
    {
        ui::ViewState view;
        std::uint64_t presentation{}, contextGeneration{};
        ui::PointerPressGate press;
        // A click counts only when its press was armed: released first in this
        // presentation and hand, as the press gate requires.
        bool pressArmed{}, wasDown{};
        void reset() { view = {}; presentation = 0; contextGeneration = 0; press = {}; pressArmed = wasDown = false; }
    };

    BackpackPanel::BackpackPanel(Kind kind, storage::Location guide) : _kind(kind), _guideLocation(guide), _renderer(std::make_unique<RenderState>()) {}
    // Plugin services live until process exit. No hot-unload or early userdata
    // destruction is allowed while RPS UI retains a callback registration.
    BackpackPanel::~BackpackPanel() = default;

    bool BackpackPanel::ensureRegistered(float width, float height) noexcept
    {
        using namespace rpsui::api;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            if (_lastResult != Result::InvalidArgument)
                logger::error("ROCK Backpack physical panel {}x{} is not a size.", width, height);
            _lastResult = Result::InvalidArgument;
            return false;
        }
        if (_retiring && !retire()) return false;
        // RPS UI sizes the texture from the shown size, so a new physical
        // size is only a new presentation.
        if (_panel) return _api->isFrameworkReady() && !_renderFailed.load();
        const auto module = GetModuleHandleW(kModuleName);
        const auto query = module ? reinterpret_cast<QueryInterfaceV1>(GetProcAddress(module, kQueryExportName)) : nullptr;
        if (acquire(query, _api) != Result::Ok || acquire(query, _cooperation) != Result::Ok || !_api->isFrameworkReady()) {
            if (_lastResult != Result::FrameworkNotReady)
                logger::warn("ROCK Backpack is waiting for RPS UI Framework panel and cooperation services.");
            _lastResult = Result::FrameworkNotReady;
            return false;
        }
        if (!_owner) {
            framework::ConsumerRegistration registration;
            std::snprintf(registration.consumerId, sizeof(registration.consumerId), _kind==Kind::PullPrompt?"rock.backpack.pull.%u":isGuidance() ? "rock.backpack.guide.%u" : (_kind == Kind::Opener) ? "rock.backpack.open" : "rock.backpack", unsigned(_guideLocation));
            std::snprintf(registration.displayName, sizeof(registration.displayName), "ROCK Backpack");
            framework::ConsumerHandle owner;
            const auto result = _api->registerConsumer(&registration, &owner);
            if (result != Result::Ok) {
                if (result != _lastResult) logger::warn("ROCK Backpack RPS UI consumer registration failed: {}", static_cast<unsigned>(result));
                _lastResult = result;
                return false;
            }
            _owner = owner.ownerToken;
        }
        cooperation::PanelRegistration registration;
        auto& panel = registration.panel;
        std::snprintf(panel.panelId, sizeof(panel.panelId), _kind==Kind::PullPrompt?"rock.backpack.pull.%u":isGuidance() ? "rock.backpack.guide.%u" : (_kind == Kind::Opener) ? "rock.backpack.open" : "rock.backpack.inventory", unsigned(_guideLocation));
        std::snprintf(panel.displayName, sizeof(panel.displayName), "ROCK Backpack");
        panel.layoutWidth = logicalWidth();
        panel.layoutHeight = logicalHeight();
        panel.defaultPhysicalWidth = width;
        panel.minimumPhysicalWidth = (_kind == Kind::Inventory) ? 8.f : .5f;
        panel.maximumPhysicalWidth = isGuidance() ? 200.f : 60.f;
        panel.flags = static_cast<unsigned>(framework::PanelFlag::FixedSize);
        if (isGuidance()) panel.flags |= static_cast<unsigned>(framework::PanelFlag::Transparent);
        panel.sortOrder = 84;
        panel.imgui = RPSUI_IMGUI_BUILD();
        panel.setupCallback = &setup;
        panel.drawCallback = &render;
        panel.userData = this;
        cooperation::PanelAgreement agreement;
        const passive_panel::Api* passive{};
        if (isGuidance()) (void)acquire(query, passive);
        const auto result = isGuidance() ? (passive ? passive->registerPanel(_owner, &panel, &agreement.panelHandle) : Result::VersionMismatch) :
            _cooperation->registerPanel(_owner, &registration, &agreement);
        if (result != Result::Ok) {
            if (result != _lastResult) logger::warn("ROCK Backpack RPS UI registration rejected: {}", static_cast<unsigned>(result));
            _lastResult = result;
            return false;
        }
        _panel = agreement.panelHandle;
        _lastResult = Result::Ok;
        _lastPresentationResult = Result::Ok;
        logger::info("ROCK Backpack registered RPS UI panel {}: physical={}x{}.", _panel, width, height);
        return true;
    }

    bool BackpackPanel::present(const panel_math::PanelPose& pose, float width, float height) noexcept
    {
        using namespace rpsui::api;
        if (!_panel || _renderFailed.load() || !_api->isFrameworkReady()) { hide(); return false; }
        framework::PanelPresentation next;
        next.sequence = ++_sequence;
        next.open = 1;
        const auto copy = [](float (&to)[3], const panel_math::Vec3& from) { to[0] = from.x; to[1] = from.y; to[2] = from.z; };
        copy(next.pose.center, pose.position); copy(next.pose.right, pose.right);
        copy(next.pose.up, pose.up); copy(next.pose.front, pose.front);
        next.pose.physicalWidth = width;
        next.pose.physicalHeight = height;
        const auto result = _api->submitPanelPresentation(_owner, _panel, &next);
        if (result != Result::Ok && result != _lastPresentationResult)
            logger::warn("ROCK Backpack RPS UI presentation rejected: {}, physical={}x{}.",
                static_cast<unsigned>(result), width, height);
        _lastPresentationResult = result;
        if (result != Result::Ok) { hide(); return false; }
        _open.store(true, std::memory_order_release);
        if (_kind == Kind::Inventory) item_preview::setVisible(true);
        return true;
    }

    void BackpackPanel::hide() noexcept
    {
        if (_kind == Kind::Inventory) item_preview::setVisible(false);
        if (_open.exchange(false, std::memory_order_acq_rel)) ++_presentation;
        if (_api && _panel) {
            rpsui::api::framework::PanelPresentation next;
            next.sequence = ++_sequence;
            const auto result = _api->submitPanelPresentation(_owner, _panel, &next);
            if (result != rpsui::api::Result::Ok && result != _lastResult)
                logger::warn("ROCK Backpack RPS UI close rejected: {}", static_cast<unsigned>(result));
            _lastResult = result;
        }
    }

    void BackpackPanel::publishGuidance(zones::Guidance guidance)
    { std::scoped_lock lock(_mailbox); _guidance = guidance; }
    void BackpackPanel::publishPullPrompt(ui::PullPrompt prompt)
    { std::scoped_lock lock(_mailbox); _pullPrompt=prompt; }

    void BackpackPanel::notifyInventoryFull() noexcept
    { _inventoryFullUntil = GetTickCount64()+2200; }

    void BackpackPanel::publish(std::shared_ptr<const ui::Model> model)
    {
        std::scoped_lock lock(_mailbox);
        _model = std::move(model);
    }
    std::optional<ui::Action> BackpackPanel::takeAction()
    {
        std::scoped_lock lock(_mailbox);
        return std::exchange(_action, {});
    }
    bool BackpackPanel::accepts(const ui::Action& action) const noexcept
    { return _open.load() && !_renderFailed.load() && action.presentation == _presentation.load(); }
    bool BackpackPanel::retire() noexcept
    {
        hide();
        if (!_owner) { _retiring = false; return true; }
        _retiring = true;
        const auto result = _cooperation->unregisterConsumerSafely(_owner);
        if (result == rpsui::api::Result::CallbackBusy) return false;
        if (result != rpsui::api::Result::Ok) {
            if (result != _lastResult) logger::error("ROCK Backpack UI retirement failed: {}", static_cast<unsigned>(result));
            _lastResult = result;
            return false;
        }
        // No callback can still run; RPS UI retired the panel's ImGui context.
        _renderer->reset();
        _owner = _panel = 0; _retiring = false; _renderFailed = false;
        { std::scoped_lock lock(_mailbox); _action.reset(); }
        return true;
    }

    float BackpackPanel::logicalWidth() const noexcept
    {
        return isGuidance() ? ui::kGuidanceWidth : float(_kind == Kind::Opener ? ui::kButtonWidth : ui::kWidth);
    }

    float BackpackPanel::logicalHeight() const noexcept
    {
        return isGuidance() ? ui::kGuidanceHeight : float(_kind == Kind::Opener ? ui::kButtonHeight : ui::kHeight);
    }

    void RPSUI_CALL BackpackPanel::setup(const rpsui::api::framework::PanelSetup* setup, void* data) noexcept
    {
        if (!setup || setup->size < sizeof(*setup) || !data) return;
        auto& panel = *static_cast<BackpackPanel*>(data);
        try {
            RPSUI_IMGUI_BIND(setup->imgui);
            ui::theme::Scope theme(setup->theme);
            ui::initializeStyle();
            panel._renderer->contextGeneration = setup->contextGeneration;
            logger::info("ROCK Backpack RPS UI panel context ready for native item pictures.");
        } catch (const std::exception& error) {
            if (!panel._renderFailed.exchange(true)) logger::error("ROCK Backpack panel setup failed: {}", error.what());
        } catch (...) {
            if (!panel._renderFailed.exchange(true)) logger::error("ROCK Backpack panel setup failed.");
        }
    }

    void RPSUI_CALL BackpackPanel::render(const rpsui::api::framework::PanelDrawFrame* frame, void* data) noexcept
    {
        if (!frame || frame->size < sizeof(*frame) || !data || !frame->d3dDevice) return;
        auto& panel = *static_cast<BackpackPanel*>(data);
        if (!panel._open.load() || panel._renderFailed.load()) return;
        try { panel.draw(*frame); }
        catch (const std::exception& error) {
            if (!panel._renderFailed.exchange(true)) logger::error("ROCK Backpack renderer failed: {}", error.what());
        } catch (...) {
            if (!panel._renderFailed.exchange(true)) logger::error("ROCK Backpack renderer failed.");
        }
    }

    void BackpackPanel::draw(const rpsui::api::framework::PanelDrawFrame& frame)
    {
        std::shared_ptr<const ui::Model> model;
        zones::Guidance guidance;
        ui::PullPrompt pullPrompt;
        { std::unique_lock lock(_mailbox, std::try_to_lock); if (!lock || (!_model && !isGuidance())) return; model = _model; guidance = _guidance; pullPrompt=_pullPrompt; }
        auto& state = *_renderer;
        if (state.contextGeneration != frame.contextGeneration) return;
        auto* device = static_cast<ID3D11Device*>(frame.d3dDevice);
        RPSUI_IMGUI_BIND(frame.imgui);
        ui::theme::Scope theme(frame.theme);
        const auto presentation = _presentation.load();
        const auto hand = !frame.pointerValid ? ui::Hand::None :
            frame.pointerHand == rpsui::api::framework::PhysicalHand::Left ? ui::Hand::Left :
            frame.pointerHand == rpsui::api::framework::PhysicalHand::Right ? ui::Hand::Right : ui::Hand::None;
        if (state.presentation != presentation) {
            state.view = {}; state.view.presentation = presentation;
            // A new presentation cancels a pressed or dragged widget before it
            // can receive the old press's release.
            ImGui::ClearActiveID();
        }
        state.presentation = presentation;
        const bool down = frame.primaryDown != 0;
        const bool pressed = state.press.update(presentation, hand, down);
        if (down && !state.wasDown) state.pressArmed = pressed;
        state.wasDown = down;
        ImGui::GetIO().MouseDrawCursor = _kind == Kind::Inventory && frame.pointerValid;
        std::optional<ui::Action> action;
        if (_kind==Kind::PullPrompt) ui::drawPullPrompt(pullPrompt);
        else if (isGuidance()) ui::drawZoneGuidance(guidance,_guideLocation);
        else if (_kind == Kind::Opener) {
            const auto now=GetTickCount64(), until=_inventoryFullUntil.load();
            if (ui::drawInventoryButton(until>now?float(until-now)/1000.f:0.f))
                action = ui::Action{.kind = ui::ActionKind::Open, .generation = model->generation, .presentation = presentation, .hand = hand};
        } else {
            const auto preview = item_preview::snapshot(device, frame.deviceGeneration);
            action = ui::drawPanel(*model, state.view, {}, hand, nullptr, preview.status,
                preview.pictures ? std::span<const ui::ItemPicture>(*preview.pictures) : std::span<const ui::ItemPicture>{});
        }
        // A click whose press was not armed never acts.
        if (action && !state.pressArmed) action.reset();
        if (action && accepts(*action)) {
            std::unique_lock lock(_mailbox, std::try_to_lock);
            if (lock && !_action) _action = std::move(action);
        }
    }
}
