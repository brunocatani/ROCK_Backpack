#pragma once
#include "PanelPoseMath.h"
#include "ui/PanelModel.h"
#include "BackpackZones.h"
#include "ui/PullPrompt.h"
#include <RPS_UI_Framework/Framework.h>
#include <RPS_UI_Framework/Cooperation.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace rock_backpack
{
    // Registration and presentation belong to the F4SE game task. The host's
    // draw callback sees immutable values and returns one bounded action.
    class BackpackPanel final
    {
    public:
        enum class Kind { Inventory, Opener, Guidance, PullPrompt };
        explicit BackpackPanel(Kind kind = Kind::Inventory, storage::Location guide = storage::Location::Interior);
        ~BackpackPanel();
        bool ensureRegistered(float width, float height) noexcept;
        bool present(const panel_math::PanelPose& pose, float width, float height) noexcept;
        void hide() noexcept;
        bool retire() noexcept;
        void publish(std::shared_ptr<const ui::Model> model);
        void publishGuidance(zones::Guidance guidance);
        void publishPullPrompt(ui::PullPrompt prompt);
        void notifyInventoryFull() noexcept;
        std::optional<ui::Action> takeAction();
        bool accepts(const ui::Action& action) const noexcept;

    private:
        bool isGuidance() const { return _kind == Kind::Guidance || _kind==Kind::PullPrompt; }
        struct RenderState;
        float logicalWidth() const noexcept;
        float logicalHeight() const noexcept;
        static void RPSUI_CALL setup(const rpsui::api::framework::PanelSetup*, void*) noexcept;
        static void RPSUI_CALL render(const rpsui::api::framework::PanelDrawFrame*, void*) noexcept;
        void draw(const rpsui::api::framework::PanelDrawFrame& frame);

        const rpsui::api::framework::Api* _api{};
        const rpsui::api::cooperation::Api* _cooperation{};
        std::uint64_t _owner{}, _panel{}, _sequence{};
        std::atomic_uint64_t _presentation{ 1 };
        std::atomic_bool _open{}, _renderFailed{};
        std::atomic_uint64_t _inventoryFullUntil{};
        bool _retiring{};
        const Kind _kind;
        const storage::Location _guideLocation;
        zones::Guidance _guidance;
        ui::PullPrompt _pullPrompt;
        rpsui::api::Result _lastResult{ rpsui::api::Result::Ok };
        rpsui::api::Result _lastPresentationResult{ rpsui::api::Result::Ok };
        std::mutex _mailbox;
        std::shared_ptr<const ui::Model> _model;
        std::optional<ui::Action> _action;
        std::unique_ptr<RenderState> _renderer;
    };
}
