# ROCK Backpack

A physical backpack for Fallout 4 VR, built on [ROCK](https://github.com/brunocatani/ROCK)'s provider APIs. You hold it in your hands, wear it on your back and pull it from your shoulder. It carries up to three weapons on outside mounts and stores items you drop into it in your normal inventory.

## Features

- Weapon mounts: a rifle on each side and a pistol below. Put a held weapon on a mount to store it, and grab it from the mount to draw it.
- Interior stash: release a held or loose item inside the backpack and it goes into your inventory.
- Inventory panel: a world-space panel on the backpack for browsing, moving, splitting, merging, using, dropping and equipping items.
- Wear and pull: return the backpack to your back and reach over your shoulder to take it out again.

## Requirements

- Fallout 4 VR with F4SE VR
- [ROCK](https://github.com/brunocatani/ROCK) 1.0 or later
- RPS UI Framework (draws the panels)

## Building

Requirements: Visual Studio 2022, CMake 4.2 or later, and vcpkg with `VCPKG_ROOT` set.

The build also needs these sources and tools, each found through a CMake cache variable:

| Variable | What it points to |
| --- | --- |
| `COMMON_LIB_F4VR_PATH` | CommonLibF4VR checkout |
| `F4VR_COMMON_FRAMEWORK_PATH` | F4VR-CommonFramework checkout |
| `ROCK_INCLUDE_DIR` | ROCK's `include/` directory |
| `RPSUI_INCLUDE_DIR` | RPS UI Framework's `include/` directory |
| `PALM_INCLUDE_DIR` | RobCo PALM's `include/` directory (icon names only; PALM is not a runtime dependency) |
| `RPSFramework_DIR` | RPS Framework's staged CMake package |
| `IMGUI_PATH` | Dear ImGui source |
| `SPRIGGIT_CLI_PATH` | Spriggit CLI, which builds `ROCK_Backpack.esp` from `plugin/ROCK_Backpack_spriggit` |

Copy `CMakeUserPresets.json.template` to `CMakeUserPresets.json`, set these paths and the mod folder (`COPY_PLUGIN_BASE_PATH`), then build:

```text
cmake --preset custom-fast
cmake --build build-fast --config Release
```

The build deploys the DLL, the `.esp` and the backpack assets to the mod folder. It also builds `ROCKBackpackPreview.exe`, a desktop preview for adjusting the weapon mounts, stash regions and panel placement without the game.

Tests are not part of the default build:

```text
cmake --build build-fast --config Release --target build_tests
ctest --test-dir build-fast -C Release --output-on-failure
```

## Layout

| Path | Contents |
| --- | --- |
| `src/` | The F4SE plugin |
| `src/ui/` | Panel model and drawing, shared with the preview |
| `preview/` | Desktop preview and its assets |
| `plugin/ROCK_Backpack_spriggit/` | Plugin records in Spriggit format |
| `assets/runtime/` | Backpack meshes, material and textures |
| `tests/` | Unit and preview tests |

## Configuration

Settings are stored in `Documents\My Games\Fallout4VR\Mods_Config\ROCK_Backpack\ROCK_Backpack.toml`. Each value is an adjustment to the shipped calibration, so zero keeps the default. The desktop preview reads and saves the same file.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
