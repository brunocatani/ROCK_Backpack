#pragma once
#include "BackpackZones.h"
#include "Theme.h"
#include <imgui.h>

namespace rock_backpack::ui
{
    inline constexpr float kGuidanceWidth=320, kGuidanceHeight=256;
    inline void drawZoneMarker(ImDrawList& draw, ImVec2 center, ImVec2 half,
        storage::Location location, bool ready, bool blocked)
    {
        if(blocked) return;
        const auto& roles=theme::current();
        const auto color=ready?theme::packed(roles.success,.96f):theme::packed(roles.text,.82f);
        const float radius=(std::min)(half.x,half.y)*.42f;
        draw.AddCircleFilled(center,radius+3,theme::packed(roles.background,.6f),32);
        draw.AddCircle(center,radius,color,32,ready?3.f:2.f);
        draw.AddLine({center.x-radius*.4f,center.y},{center.x+radius*.4f,center.y},color,2.f);
        draw.AddLine({center.x,center.y-radius*.4f},{center.x,center.y+radius*.4f},color,2.f);
        // Preserve the authored marker frame independently of stash calibration.
        for (float x:{-1.f,1.f}) for (float y:{-1.f,1.f}) {
            const ImVec2 corner{center.x+x*half.x,center.y+y*half.y};
            draw.AddLine(corner,{corner.x-x*half.x*.25f,corner.y},color,2.f);
            draw.AddLine(corner,{corner.x,corner.y-y*half.y*.25f},color,2.f);
        }
        const char* label=ready?"RELEASE TO STORE":location==storage::Location::Interior?"STASH":location==storage::Location::Pistol?"PISTOL":"RIFLE";
        const auto size=ImGui::CalcTextSize(label);
        const ImVec2 text{center.x-size.x*.5f,center.y+radius+5};
        draw.AddRectFilled({text.x-4,text.y-2},{text.x+size.x+4,text.y+size.y+2},theme::packed(roles.background,.75f),roles.rounding);
        draw.AddText(text,color,label);
    }
    inline void drawZoneGuidance(const zones::Guidance& guidance, storage::Location location)
    {
        auto& draw=*ImGui::GetBackgroundDrawList();
        const auto bit=1u<<unsigned(location);
        if (!(guidance.visible&bit)) return;
        drawZoneMarker(draw,{kGuidanceWidth*.5f,kGuidanceHeight*.5f},{kGuidanceWidth*.5f-2,kGuidanceHeight*.5f-2},location,
            (guidance.ready&bit)!=0,(guidance.blocked&bit)!=0);
    }
}
