#pragma once
#include "Theme.h"
#include <cfloat>
#include <imgui.h>

namespace rock_backpack::ui
{
    // Prompt for taking the backpack off the back over the left shoulder.
    struct PullPrompt {
        enum class Action { GripBag, PullBag, RearmBag } action{Action::GripBag};
    };
    inline void drawPullPrompt(const PullPrompt& prompt,ImVec2 origin={})
    {
        auto& draw=*ImGui::GetBackgroundDrawList();
        const ImU32 color=theme::packed(theme::current().text,.96f);
        const ImU32 shadow=theme::packed(theme::current().background,.75f);
        const auto at=[&](float x,float y) { return ImVec2{origin.x+x,origin.y+y}; };
        const ImVec2 diamond[]{at(160,23),at(169,34),at(160,45),at(151,34),at(160,23)};
        draw.AddPolyline(diamond,5,shadow,5.f,ImDrawFlags_None);
        draw.AddPolyline(diamond,5,color,2.f,ImDrawFlags_None);
        draw.AddLine(at(27,34),at(63,34),color,2); draw.AddLine(at(257,34),at(293,34),color,2);
        auto* font=ImGui::GetFont();
        const auto centered=[&](const char* text,float y,float size) {
            const auto measured=font->CalcTextSizeA(size,FLT_MAX,280,text);
            const ImVec2 position=at((320-measured.x)*.5f,y);
            draw.AddText(font,size,{position.x+1,position.y+2},shadow,text,nullptr,280);
            draw.AddText(font,size,position,color,text,nullptr,280);
            return measured.y;
        };
        const float height=centered("Backpack",62,27);
        const char* action="GRIP TO PULL";
        switch(prompt.action) {
        case PullPrompt::Action::GripBag: action="GRIP TO PULL"; break;
        case PullPrompt::Action::PullBag: action="HOLD GRIP / PULL FORWARD"; break;
        case PullPrompt::Action::RearmBag: action="RELEASE GRIP / GRIP HERE"; break;
        }
        centered(action,70+height,17);
        centered("LEFT SHOULDER",105+height,14);
    }
}
