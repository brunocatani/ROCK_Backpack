#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <cstdint>

namespace rock_backpack::preview_recovery
{
    // Checked only by the existing native-dispatch service. No timers or
    // background work; retries stop when neither output is requested.
    struct Retry {
        unsigned failures{};
        std::uint64_t after{};
        void failed(std::uint64_t now) noexcept {
            failures=(std::min)(failures+1,6u);
            after=now+(std::min)(std::uint64_t(30000),std::uint64_t(1000)<<(failures-1));
        }
        bool due(std::uint64_t now) const noexcept { return now>=after; }
    };
    constexpr bool needed(bool scene,bool visible,bool hasVisual,bool hasImage) noexcept
    { return (scene && !hasVisual) || (visible && !hasImage); }
    template<std::size_t N>
    std::size_t distinctProperties(std::array<void*,N>& properties,std::size_t count) {
        std::sort(properties.begin(),properties.begin()+count,std::less<void*>{});
        return std::size_t(std::unique(properties.begin(),properties.begin()+count)-properties.begin());
    }
    inline constexpr std::size_t kMaximumNodes=1024;
    inline constexpr std::size_t kMaximumProperties=2*kMaximumNodes;
}
