#pragma once
#include "PanelModel.h"
#include <filesystem>
#include <optional>

namespace rock_backpack::ui
{
    bool savePreviewInventory(const Model&, const std::filesystem::path&);
    std::optional<Model> loadPreviewInventory(const std::filesystem::path&);
}
