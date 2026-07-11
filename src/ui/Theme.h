#pragma once
#include <RPS_UI_Framework/Framework.h>
#include <imgui.h>

// The backpack's panels draw with the player's RPS UI theme. A Scope makes the
// theme of the running setup or draw callback current (render thread);
// current() is RPS UI's built-in theme outside a callback, which the desktop
// preview uses.
namespace rock_backpack::ui::theme
{
    void bind(const rpsui::api::framework::Theme* value) noexcept;
    [[nodiscard]] const rpsui::api::framework::Theme& current() noexcept;

    [[nodiscard]] inline ImU32 packed(const rpsui::api::framework::ThemeColor& value, float alpha = 1.f) noexcept
    {
        return ImGui::ColorConvertFloat4ToU32({ value.r, value.g, value.b, value.a * alpha });
    }

    class Scope
    {
    public:
        explicit Scope(const rpsui::api::framework::Theme* value) noexcept { bind(value); }
        ~Scope() { bind(nullptr); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
