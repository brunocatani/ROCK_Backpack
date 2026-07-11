#include <Windows.h>
#include <spdlog/spdlog.h>
#include <array>
#include <algorithm>

#include "BackpackConfig.h"

#include <ShlObj.h>

#include <charconv>
#include <cmath>
#include <format>

namespace rock_backpack
{
    namespace
    {
        namespace settings = RPS::Settings;
        constexpr std::uintmax_t kMaximumFileBytes = 64 * 1024;
        constexpr std::string_view kLegacyFileName = "ROCKBackpack.ini";
        // TOML tables, the INI sections of earlier versions and their headings.
        constexpr std::array<std::string_view, 9> kTables{ "Panel", "InventoryButton", "MountLeftRifle", "MountRightRifle",
            "MountPistol", "StashInterior", "StashLeftRifle", "StashRightRifle", "StashPistol" };
        constexpr std::array<std::string_view, 9> kOldSections{ "Panel", "InventoryButton", "Mount.LeftRifle",
            "Mount.RightRifle", "Mount.Pistol", "Stash.Interior", "Stash.LeftRifle", "Stash.RightRifle", "Stash.Pistol" };
        constexpr std::array<std::string_view, 9> kCategories{ "01. Panel", "02. Inventory button", "03. Left rifle mount",
            "04. Right rifle mount", "05. Pistol mount", "06. Interior stash region", "07. Left rifle stash region",
            "08. Right rifle stash region", "09. Pistol stash region" };
        struct Name { std::string_view key, oldKey, help; };
        constexpr std::array<Name, 6> kPositionNames{ { { "X", "fX", "Weapon position along X" },
            { "Y", "fY", "Weapon position along Y" }, { "Z", "fZ", "Weapon position along Z" },
            { "RotateXDegrees", "fRotateXDegrees", "Weapon rotation about X, in degrees" },
            { "RotateYDegrees", "fRotateYDegrees", "Weapon rotation about Y, in degrees" },
            { "RotateZDegrees", "fRotateZDegrees", "Weapon rotation about Z, in degrees" } } };
        constexpr std::array<Name, 6> kStashNames{ { { "X", "fX", "Region centre along X" },
            { "Y", "fY", "Region centre along Y" }, { "Z", "fZ", "Region centre along Z" },
            { "Width", "fWidth", "Region width" }, { "Height", "fHeight", "Region height" },
            { "Depth", "fDepth", "Region depth" } } };

        // One calibration value: the file holds an adjustment to the shipped
        // calibration (times units), the settings hold the adjusted value.
        struct Field { unsigned table; Name name; float* value; float minimum, maximum, units{ 1 }; };
        std::array<Field, 53> fields(BackpackSettings& settings)
        {
            std::array<Field, 53> result{}; unsigned count{};
            const auto add = [&](unsigned table, Name name, float& value, float minimum, float maximum, float units = 1.f) {
                result[count++] = { table, name, &value, minimum, maximum, units };
            };
            add(0, { "PanelWidth", "fPanelWidth", "Panel width" }, settings.panelPhysicalWidth,
                kMinimumPanelPhysicalDimensionGameUnits, kMaximumPanelPhysicalDimensionGameUnits);
            add(0, { "PanelHeight", "fPanelHeight", "Panel height" }, settings.panelPhysicalHeight,
                kMinimumPanelPhysicalDimensionGameUnits, kMaximumPanelPhysicalDimensionGameUnits);
            add(0, { "OffsetX", "fOffsetX", "Panel position along X" }, settings.localOffset.x, -100, 100);
            add(0, { "OffsetY", "fOffsetY", "Panel position along Y" }, settings.localOffset.y, -100, 100);
            add(0, { "OffsetZ", "fOffsetZ", "Panel position along Z" }, settings.localOffset.z, -100, 100);
            add(0, { "YawDegrees", "fYawDegrees", "Panel yaw, in degrees" }, settings.panelYawDegrees, -60, 60);
            add(1, { "OffsetX", "fOffsetX", "Button position along X" }, settings.buttonOffset.x, -100, 100);
            add(1, { "OffsetY", "fOffsetY", "Button position along Y" }, settings.buttonOffset.y, -100, 100);
            add(1, { "OffsetZ", "fOffsetZ", "Button position along Z" }, settings.buttonOffset.z, -100, 100);
            add(1, { "Width", "fWidth", "Button width" }, settings.buttonWidth, 1, 60);
            add(1, { "Height", "fHeight", "Button height" }, settings.buttonHeight, 1, 60);
            for (unsigned i = 0; i < 3; ++i) {
                auto& p = settings.mounts.positions[i]; auto& r = settings.mounts.rotations[i];
                float* values[]{ &p.x, &p.y, &p.z, &r.x, &r.y, &r.z };
                for (unsigned axis = 0; axis < 6; ++axis)
                    add(2 + i, kPositionNames[axis], *values[axis], axis < 3 ? -100.f : -360.f, axis < 3 ? 100.f : 360.f);
            }
            for (unsigned i = 0; i < 4; ++i) {
                auto& p = settings.stash.centers[i]; auto& h = settings.stash.halves[i];
                float* values[]{ &p.x, &p.y, &p.z, &h.x, &h.y, &h.z };
                for (unsigned axis = 0; axis < 6; ++axis)
                    add(5 + i, kStashNames[axis], *values[axis], axis < 3 ? -100.f : .5f, axis < 3 ? 100.f : 50.f, axis < 3 ? 1.f : 2.f);
            }
            return result;
        }

        std::string numberText(float value)
        {
            std::array<char, 32> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            return settings::normalizeValue(settings::ValueType::Float,
                std::string_view(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data()))).value_or("0.0");
        }

        std::vector<settings::Definition> definitions()
        {
            BackpackSettings defaults; const auto catalog = fields(defaults);
            std::vector<settings::Definition> result;
            result.push_back({ std::string(kTables[0]), "Enabled", std::string(kCategories[0]), "Shows the backpack panel.",
                settings::Group::Player, settings::ValueType::Boolean, "true" });
            for (const auto& field : catalog) {
                // Sizes of regions are stored as full extents around half-size settings.
                const auto minimum = field.minimum * field.units, maximum = field.maximum * field.units;
                const auto actual = *field.value * field.units;
                result.push_back({ std::string(kTables[field.table]), std::string(field.name.key),
                    std::string(kCategories[field.table]),
                    std::format("{}: adjustment to the shipped {}; 0 keeps it. The result must stay within {} to {}.",
                        field.name.help, numberText(actual), numberText(minimum), numberText(maximum)),
                    settings::Group::Player, settings::ValueType::Float, "0.0" });
            }
            return result;
        }

        // The INI of earlier versions held the same adjustments; all defaulted to 0.
        std::span<const settings::LegacyKey> legacyKeys()
        {
            static const auto keys = [] {
                BackpackSettings defaults; const auto catalog = fields(defaults);
                std::vector<settings::LegacyKey> result;
                settings::LegacyKey enabled{};
                enabled.oldKey = "bEnabled"; enabled.table = kTables[0]; enabled.key = "Enabled";
                enabled.oldDefault = "true"; enabled.oldSection = kOldSections[0];
                result.push_back(enabled);
                for (const auto& field : catalog) {
                    settings::LegacyKey key{};
                    key.oldKey = field.name.oldKey; key.table = kTables[field.table]; key.key = field.name.key;
                    key.oldDefault = "0"; key.oldSection = kOldSections[field.table];
                    result.push_back(key);
                }
                return result;
            }();
            return keys;
        }

        std::unique_ptr<settings::Store> makeStore(const std::filesystem::path& path)
        {
            const auto directory = path.parent_path();
            // Only calibration format 2 holds adjustments; an older INI of
            // absolute values is never read as adjustments and stays untouched.
            std::vector<std::filesystem::path> legacy;
            const auto ini = directory / kLegacyFileName;
            if (GetPrivateProfileIntW(L"Panel", L"iCalibrationVersion", 0, ini.c_str()) == 2)
                legacy.push_back(ini);
            return std::make_unique<settings::Store>(
                settings::Files{ directory, "ROCK Backpack", path.filename().string(), {},
                    "## ROCK_Backpack.toml - ROCK Backpack placement calibration\n"
                    "## Every number is an adjustment to the shipped calibration: 0 keeps the shipped position, angle\n"
                    "## or size. Positions and sizes are game units, rotations degrees. Changes apply live. A value\n"
                    "## that takes its setting out of range keeps every setting at its last valid value until fixed.\n"
                    "## Your own notes are kept when they start with a single # on the lines above an option.\n",
                    {} },
                definitions(), settings::LegacyIni{ std::move(legacy), legacyKeys() });
        }

        std::optional<BackpackSettings> fromStore(const settings::Store& store, std::string& error)
        {
            BackpackSettings result, baseline; const auto catalog = fields(result), base = fields(baseline);
            const auto* enabled = store.find(kTables[0], "Enabled");
            if (!enabled) { error = "Enabled is not declared"; return {}; }
            result.panelEnabled = enabled->value == "true";
            for (unsigned i = 0; i < catalog.size(); ++i) {
                const auto& field = catalog[i];
                const auto* setting = store.find(kTables[field.table], field.name.key);
                float adjustment{};
                const auto& text = setting ? setting->value : std::string{};
                const auto read = std::from_chars(text.data(), text.data() + text.size(), adjustment);
                if (!setting || read.ec != std::errc{}) { error = std::format("[{}] {} is unreadable", kTables[field.table], field.name.key); return {}; }
                const float actual = *base[i].value + adjustment / field.units;
                if (!std::isfinite(actual) || actual < field.minimum || actual > field.maximum) {
                    error = std::format("[{}] {} = {} exceeds the physical limits", kTables[field.table], field.name.key, text);
                    return {};
                }
                *field.value = actual;
            }
            return result;
        }

        bool loadStore(settings::Store& store, const std::filesystem::path& path, std::string& error)
        {
            std::error_code ignored;
            // The first use creates the file and migrates the INI, as the game's first start does.
            const bool create = !std::filesystem::exists(path, ignored);
            store.load(create);
            // A problem in an existing file (unparsable, a value of the wrong type)
            // refuses it, so an editor never saves defaults over the player's text.
            if (!create && !store.messages().empty()) { error = store.messages().front(); return false; }
            return true;
        }
    }

    std::filesystem::path BackpackConfig::resolvePath() noexcept
    {
        PWSTR documents = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) || !documents) {
            if (documents) {
                CoTaskMemFree(documents);
            }
            return {};
        }
        std::filesystem::path path{ documents };
        CoTaskMemFree(documents);
        return path / "My Games" / "Fallout4VR" / "Mods_Config" / "ROCK_Backpack" / kFileName;
    }

    std::optional<BackpackConfig::FileStamp> BackpackConfig::captureStamp(const std::filesystem::path& path) noexcept
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > kMaximumFileBytes) {
            return std::nullopt;
        }
        const auto writeTime = std::filesystem::last_write_time(path, error);
        if (error) {
            return std::nullopt;
        }
        return FileStamp{ writeTime, size };
    }

    std::optional<BackpackSettings> BackpackConfig::readOnly(std::string& error, const std::filesystem::path& requested) noexcept
    {
        try {
            const auto path = requested.empty() ? resolvePath() : requested;
            if (path.empty()) { error = "Documents folder is unavailable"; return {}; }
            std::filesystem::create_directories(path.parent_path());
            auto store = makeStore(path);
            if (!loadStore(*store, path, error)) return {};
            return fromStore(*store, error);
        } catch (const std::exception& e) { error = e.what(); return {}; }
        catch (...) { error = "Calibration settings could not be read."; return {}; }
    }

    bool BackpackConfig::saveSettings(const BackpackSettings& requested, std::string& error, const std::filesystem::path& outputPath) noexcept
    {
        try {
            error.clear();
            auto settings = requested; BackpackSettings baseline;
            const auto catalog = fields(settings), base = fields(baseline);
            for (const auto& field : catalog) if (!std::isfinite(*field.value) || *field.value < field.minimum || *field.value > field.maximum) {
                error = std::format("Calibration exceeds physical limits: [{}] {}", kTables[field.table], field.name.key); return false;
            }
            const auto path = outputPath.empty() ? resolvePath() : outputPath;
            if (path.empty() || !path.is_absolute()) { error = "Absolute settings path is unavailable"; return false; }
            std::filesystem::create_directories(path.parent_path());
            auto store = makeStore(path);
            // A file that does not read or holds out-of-range values is never overwritten.
            if (!loadStore(*store, path, error) || !fromStore(*store, error)) return false;
            std::vector<std::string> values;
            values.reserve(catalog.size() + 1);
            std::vector<settings::Change> changes;
            values.push_back(settings.panelEnabled ? "true" : "false");
            changes.push_back({ kTables[0], "Enabled", values.back() });
            for (unsigned i = 0; i < catalog.size(); ++i) {
                values.push_back(numberText((*catalog[i].value - *base[i].value) * catalog[i].units));
                changes.push_back({ kTables[catalog[i].table], catalog[i].name.key, values.back() });
            }
            if (!store->setValues(settings::Group::Player, changes)) { error = store->error(); return false; }
            return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
        catch (...) { error = "Calibration settings could not be saved."; return false; }
    }

    void BackpackConfig::reload(const bool initialLoad) noexcept
    {
        std::string error;
        std::optional<BackpackSettings> parsed;
        try {
            _store->load(initialLoad);
            for (const auto& notice : _store->notices()) spdlog::info("ROCK Backpack config: {}", notice);
            for (const auto& message : _store->messages()) spdlog::warn("ROCK Backpack config: {}", message);
            parsed = fromStore(*_store, error);
        } catch (const std::exception& e) { error = e.what(); }
        // A startup load can rewrite the file; watch from what it left.
        _loadedStamp = captureStamp(_path);
        if (!parsed) {
            spdlog::warn("ROCK Backpack config {} rejected; retaining last valid settings ({}).",
                initialLoad ? "load" : "reload", error);
            return;
        }
        publish(*parsed);
        _missingFileLogged = false;
        spdlog::info(
            "ROCK Backpack config {}: enabled={}, panel={:.2f}x{:.2f}, local offset=({:.2f}, {:.2f}, {:.2f}).",
            initialLoad ? "loaded" : "reloaded", parsed->panelEnabled,
            parsed->panelPhysicalWidth, parsed->panelPhysicalHeight,
            parsed->localOffset.x, parsed->localOffset.y, parsed->localOffset.z);
    }

    void BackpackConfig::initialize() noexcept
    {
        if (_initialized) {
            return;
        }
        _initialized = true;
        _path = resolvePath();
        try {
            if (!_path.empty()) {
                std::filesystem::create_directories(_path.parent_path());
                _store = makeStore(_path);
            }
        } catch (const std::exception& e) {
            spdlog::error("ROCK Backpack settings unavailable: {}", e.what());
            _store.reset();
        }
        if (!_store) {
            spdlog::error("ROCK Backpack could not resolve {}; compiled defaults remain active.", kFileName);
            _path.clear();
            return;
        }
        // The startup load creates the file when missing and migrates the INI.
        reload(true);
        std::error_code ignored;
        if (std::filesystem::exists(_path.parent_path() / kLegacyFileName, ignored)) {
            spdlog::warn("ROCK Backpack: {} uses the retired absolute calibration format and was not migrated; "
                "its values are ignored.", kLegacyFileName);
        }
    }

    void BackpackConfig::poll() noexcept
    {
        if (!_initialized) {
            initialize();
        }
        if (_path.empty()) {
            return;
        }
        const auto stamp = captureStamp(_path);
        if (!stamp) {
            if (!_missingFileLogged) {
                spdlog::warn("ROCK Backpack cannot stat {}; retaining last valid settings.", kFileName);
                _missingFileLogged = true;
            }
            return;
        }
        if (!_loadedStamp || *stamp != *_loadedStamp) {
            reload(false);
        }
    }

    void BackpackConfig::publish(const BackpackSettings& settings) noexcept
    {
        _sequence.fetch_add(1, std::memory_order_acq_rel);
        _panelEnabled.store(settings.panelEnabled, std::memory_order_relaxed);
        _panelYawDegrees.store(settings.panelYawDegrees,std::memory_order_relaxed);
        _offsetX.store(settings.localOffset.x, std::memory_order_relaxed);
        _offsetY.store(settings.localOffset.y, std::memory_order_relaxed);
        _offsetZ.store(settings.localOffset.z, std::memory_order_relaxed);
        _panelPhysicalWidth.store(settings.panelPhysicalWidth, std::memory_order_relaxed);
        _panelPhysicalHeight.store(settings.panelPhysicalHeight, std::memory_order_relaxed);
        _buttonX = settings.buttonOffset.x; _buttonY = settings.buttonOffset.y; _buttonZ = settings.buttonOffset.z;
        _buttonWidth = settings.buttonWidth; _buttonHeight = settings.buttonHeight;
        for(unsigned i=0;i<3;++i) {
            const auto& p=settings.mounts.positions[i];
            const auto& r=settings.mounts.rotations[i];
            const float values[]{p.x,p.y,p.z,r.x,r.y,r.z};
            for(unsigned axis=0;axis<6;++axis) _mountPositions[i*6+axis].store(values[axis],std::memory_order_relaxed);
        }
        for(unsigned i=0;i<4;++i) {
            const auto& p=settings.stash.centers[i]; const auto& h=settings.stash.halves[i];
            const float values[]{p.x,p.y,p.z,h.x,h.y,h.z};
            for(unsigned axis=0;axis<6;++axis) _stashRegions[i*6+axis].store(values[axis],std::memory_order_relaxed);
        }
        _sequence.fetch_add(1, std::memory_order_release);
    }

    BackpackSettings BackpackConfig::snapshot() const noexcept
    {
        BackpackSettings settings{};
        for (;;) {
            const auto before = _sequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }
            settings.panelYawDegrees = _panelYawDegrees.load(std::memory_order_relaxed);
            settings.panelEnabled = _panelEnabled.load(std::memory_order_relaxed);
            settings.localOffset = {
                _offsetX.load(std::memory_order_relaxed),
                _offsetY.load(std::memory_order_relaxed),
                _offsetZ.load(std::memory_order_relaxed),
            };
            settings.panelPhysicalWidth =
                _panelPhysicalWidth.load(std::memory_order_relaxed);
            settings.panelPhysicalHeight =
                _panelPhysicalHeight.load(std::memory_order_relaxed);
            settings.buttonOffset = {_buttonX.load(),_buttonY.load(),_buttonZ.load()};
            settings.buttonWidth = _buttonWidth.load(); settings.buttonHeight = _buttonHeight.load();
            for(unsigned i=0;i<3;++i) {
                auto& p=settings.mounts.positions[i]; auto& r=settings.mounts.rotations[i];
                float* values[]{&p.x,&p.y,&p.z,&r.x,&r.y,&r.z};
                for(unsigned axis=0;axis<6;++axis) *values[axis]=_mountPositions[i*6+axis].load(std::memory_order_relaxed);
            }
            for(unsigned i=0;i<4;++i) {
                auto& p=settings.stash.centers[i]; auto& h=settings.stash.halves[i];
                float* values[]{&p.x,&p.y,&p.z,&h.x,&h.y,&h.z};
                for(unsigned axis=0;axis<6;++axis) *values[axis]=_stashRegions[i*6+axis].load(std::memory_order_relaxed);
            }
            const auto after = _sequence.load(std::memory_order_acquire);
            if (before == after) {
                return settings;
            }
        }
    }
}
