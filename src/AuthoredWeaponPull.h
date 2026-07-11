#pragma once
#include "PanelPoseMath.h"
#include <cstdint>

namespace rock_backpack::authored_pull
{
    using panel_math::Vec3;
    inline constexpr float kUnlockDistanceGame=6.f;
    inline constexpr float kDiscontinuityDistanceGame=50.f;
    struct Key {
        std::uint64_t item{},display{},pose{};
        std::uint32_t reference{},parent{},role{},world{},skeleton{},provider{};
        std::uint64_t stack{},instance{};
        bool operator==(const Key&) const = default;
    };
    enum class Phase { Idle, Hover, Latched, Unlock, Cancelled };
    struct Input {
        bool available{},held{},valid{};
        Key key;
        Vec3 controller{}; // Current display-local tracked controller, not posed wrist.
        float scale{1},distance{},radius{};
        Vec3 tracking{}; // Tracking-room game units, unaffected by locomotion.
    };
    struct Gesture {
        Key key;
        Vec3 start{},previous{},trackingStart{},trackingPrevious{};
        float scale{1};
        bool primed{},down{},latched{},pending{},consumed{};
        void cancel() { key={}; latched=pending=false; }
        void acknowledge() { cancel(); consumed=down; }
        Phase update(const Input& input)
        {
            if(!input.available) { *this={}; return Phase::Idle; }
            const bool pressed=primed && !down && input.held;
            down=input.held;
            if(!primed) { primed=!down; return Phase::Idle; }
            if(consumed) { if(!down) consumed=false; return Phase::Idle; }
            const bool valid=input.valid && input.key.item && panel_math::isFinite(input.controller) && panel_math::isFinite(input.tracking) &&
                std::isfinite(input.scale) && input.scale>.0001f && std::isfinite(input.distance) && std::isfinite(input.radius);
            if(latched) {
                if(!valid || input.key!=key || !down || std::abs(input.scale-scale)>.001f) { cancel(); return Phase::Cancelled; }
                const auto step=(input.controller-previous)*scale;
                const auto trackedStep=input.tracking-trackingPrevious;
                if(panel_math::dot(step,step)>kDiscontinuityDistanceGame*kDiscontinuityDistanceGame ||
                    panel_math::dot(trackedStep,trackedStep)>kDiscontinuityDistanceGame*kDiscontinuityDistanceGame) { cancel(); return Phase::Cancelled; }
                previous=input.controller;
                trackingPrevious=input.tracking;
                const auto pull=(input.controller-start)*scale;
                const auto travel=input.tracking-trackingStart;
                pending|=panel_math::dot(pull,pull)>=kUnlockDistanceGame*kUnlockDistanceGame &&
                    panel_math::dot(travel,travel)>=kUnlockDistanceGame*kUnlockDistanceGame;
                return pending?Phase::Unlock:Phase::Latched;
            }
            if(!valid || input.distance>input.radius || input.radius<=0) return Phase::Idle;
            if(!pressed) return Phase::Hover;
            key=input.key; start=previous=input.controller; trackingStart=trackingPrevious=input.tracking; scale=input.scale; latched=true;
            return Phase::Latched;
        }
    };
}
