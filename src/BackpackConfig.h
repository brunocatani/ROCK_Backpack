#pragma once

#include "PanelPoseMath.h"
#include "BackpackZones.h"

#include <RPS/Settings/Store.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace rock_backpack
{
    // Captured physical panel size. RPS UI scales the layout onto it.
    inline constexpr float kDefaultPanelPhysicalWidthGameUnits = 40.5f;
    inline constexpr float kDefaultPanelPhysicalHeightGameUnits = 30.0f;
    inline constexpr float kMinimumPanelPhysicalDimensionGameUnits = 8.0f;
    inline constexpr float kMaximumPanelPhysicalDimensionGameUnits = 60.0f;

    struct BackpackSettings
    {
        bool panelEnabled{ true };
        panel_math::Vec3 localOffset{0,0,10};
        float panelPhysicalWidth{ kDefaultPanelPhysicalWidthGameUnits };
        float panelPhysicalHeight{ kDefaultPanelPhysicalHeightGameUnits };
        float panelYawDegrees{45};
        panel_math::Vec3 buttonOffset{0,-12,6};
        float buttonWidth{10}, buttonHeight{3};
        // The user's captured calibration is the shipped zero-adjustment pose.
        // Visual placement and authored stash regions are independently calibrated.
        zones::Layout mounts{{{{12.592592f,6.923501f,8},{-12.814317f,9.212240f,8},{3.016674f,17,8}}},
            {{{0,0,0},{0,0,0},{0,-33.025402f,0}}}};
        zones::StashLayout stash;
    };

    // ROCK_Backpack.toml through RPS Framework's settings store; it holds
    // adjustments to the shipped calibration. All filesystem work is owned by
    // BackpackRuntime's scheduler thread. The game task reads a lock-free
    // seqlock snapshot and never performs I/O.
    class BackpackConfig final
    {
    public:
        static constexpr std::string_view kFileName = "ROCK_Backpack.toml";

        BackpackConfig() noexcept { publish(BackpackSettings{}); }
        // Reads a settings file (default: the player's). A missing file is
        // created first, migrating ROCKBackpack.ini as the game's first start does.
        static std::optional<BackpackSettings> readOnly(std::string& error, const std::filesystem::path& path = {}) noexcept;
        // Writes every calibration value; refuses values out of range and a
        // file that does not read, leaving it unchanged.
        static bool saveSettings(const BackpackSettings&, std::string& error, const std::filesystem::path& path = {}) noexcept;
        void initialize() noexcept;
        void poll() noexcept;
        [[nodiscard]] BackpackSettings snapshot() const noexcept;

    private:
        struct FileStamp
        {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size{ 0 };

            [[nodiscard]] bool operator==(const FileStamp&) const = default;
        };

        [[nodiscard]] static std::filesystem::path resolvePath() noexcept;
        [[nodiscard]] static std::optional<FileStamp> captureStamp(const std::filesystem::path& path) noexcept;
        void publish(const BackpackSettings& settings) noexcept;
        void reload(bool initialLoad) noexcept;

        std::unique_ptr<RPS::Settings::Store> _store;
        std::filesystem::path _path;
        std::optional<FileStamp> _loadedStamp;
        bool _initialized{ false };
        bool _missingFileLogged{ false };

        mutable std::atomic_uint64_t _sequence{ 0 };
        std::atomic_bool _panelEnabled{ true };
        std::atomic<float> _offsetX{ 0.0f };
        std::atomic<float> _offsetY{ 0.0f };
        std::atomic<float> _offsetZ{ 0.0f };
        std::atomic<float> _panelPhysicalWidth{ kDefaultPanelPhysicalWidthGameUnits };
        std::atomic<float> _panelPhysicalHeight{ kDefaultPanelPhysicalHeightGameUnits };
        std::atomic<float> _panelYawDegrees{};
        std::atomic<float> _buttonX{}, _buttonY{}, _buttonZ{10}, _buttonWidth{18}, _buttonHeight{6};
        std::array<std::atomic<float>,18> _mountPositions{};
        std::array<std::atomic<float>,24> _stashRegions{};
    };
}
