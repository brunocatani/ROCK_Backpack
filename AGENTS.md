# AGENTS.md — ROCK_Backpack

ROCK_Backpack is the backpack addon, a consumer of ROCK's provider APIs. ROCK ownership and API rules come from the ROCK suite workspace instructions.

- Deploys to the mod folder set by `COPY_PLUGIN_BASE_PATH` in `CMakeUserPresets.json`.
- Config: `Documents\My Games\Fallout4VR\Mods_Config\ROCK_Backpack\ROCK_Backpack.toml`, written through RPS Framework's settings store (`RPSFramework_DIR` names its staged package). Every number is an adjustment to the shipped calibration. The first start migrates a format-2 `ROCKBackpack.ini` and deletes it; an older absolute-format INI is left unread. Item preview images live under `Mods_Config\ROCKBackpack\ItemPreviews\`.
- The desktop preview reads and saves the same file; opening it before the game creates the file the same way.
- Backpack policy stays in this addon. Change ROCK only when the task explicitly identifies a provider-contract need.
