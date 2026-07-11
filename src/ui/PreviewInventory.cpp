#include "PreviewInventory.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <cmath>

namespace rock_backpack::ui
{
    bool savePreviewInventory(const Model& model, const std::filesystem::path& path)
    {
        try {
            if (path.empty() || !path.is_absolute() || !model.available) return false;
            nlohmann::json data{{"schema",4},{"columns",model.grid.columns()},{"rows",model.grid.rows()}};
            data["items"] = nlohmann::json::array();
            for (const auto& item : model.items) if (const auto* entry = model.grid.find(item.storageId)) {
                data["items"].push_back({{"id",entry->id},{"appearance",item.appearance},{"name",item.name},{"form",item.formId},
                    {"kind",unsigned(entry->kind)},{"category",unsigned(item.category)},{"count",entry->count},{"stack",entry->stackKey},
                    {"location",unsigned(entry->placement.location)},{"x",entry->placement.x},{"y",entry->placement.y},
                    {"rotated",entry->placement.rotated},{"weight",item.weight},{"usable",item.usable},{"equipped",item.equipped}});
            }
            std::filesystem::create_directories(path.parent_path());
            auto temporary = path; temporary += L".tmp";
            { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc); stream << data.dump(2); if (!stream) return false; }
            return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        } catch (...) { return false; }
    }
    std::optional<Model> loadPreviewInventory(const std::filesystem::path& path)
    {
        try {
            std::error_code error;
            const auto bytes = std::filesystem::file_size(path, error);
            if (error || !bytes) return {};
            std::ifstream stream(path, std::ios::binary);
            const auto data = nlohmann::json::parse(stream, nullptr, false);
            if (data.is_discarded() || (data.at("schema") != 2 && data.at("schema") != 3 && data.at("schema") != 4)) return {};
            const auto columns = data.at("columns").get<unsigned>(), rows = data.at("rows").get<unsigned>();
            if (!columns || !rows || columns > 32 || rows > 32 || !data.at("items").is_array()) return {};
            Model result; result.generation = 1; result.available = true;
            result.grid = storage::Grid(columns, rows);
            result.status = "CAPTURED BACKPACK / DESKTOP SIMULATION  -  F5 reloads the last captured inventory and models.";
            for (const auto& value : data.at("items")) {
                Item item;
                item.storageId = value.at("id"); item.appearance = value.at("appearance");
                item.id = std::to_string(item.storageId); item.name = value.at("name"); item.formId = value.at("form");
                item.count = value.at("count"); item.weight = value.at("weight"); item.usable = value.at("usable");
                item.equipped=value.value("equipped",false);
                const auto kind = value.at("kind").get<unsigned>(), category = value.at("category").get<unsigned>(), location = value.at("location").get<unsigned>();
                if (kind > unsigned(storage::Kind::Knife) || category >= 6 || location > unsigned(data.at("schema")==4?storage::Location::Equipped:data.at("schema")==3?storage::Location::Overflow:storage::Location::RetiredLegHolster) ||
                    item.name.size() > 512 || !std::isfinite(item.weight) || item.weight < 0) return {};
                item.category = inventory_policy::Category(category);
                storage::Entry entry{item.storageId, storage::Kind(kind), item.count, value.at("stack").get<std::uint64_t>(),
                    {storage::savedLocation(std::uint8_t(location)), value.at("x").get<unsigned>(), value.at("y").get<unsigned>(), value.at("rotated").get<bool>()}};
                if (result.grid.insert(entry) != storage::Result::Ok) return {};
                result.weight += item.weight * item.count; result.items.push_back(std::move(item));
            }
            result.indexItems(); return result;
        } catch (...) { return {}; }
    }
}
