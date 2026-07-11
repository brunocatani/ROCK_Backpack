#pragma once
#include "StorageGrid.h"

namespace rock_backpack::inventory_capacity
{
    enum class Source { Voluntary, ExistingInventory, ScriptedGrant };
    struct Admission {
        bool accepted{}, merge{};
        std::optional<storage::Placement> placement;
    };
    // Classification is supplied by the acquisition boundary, never inferred
    // from a late inventory notification. That would grandfather refused takes.
    inline Admission evaluate(const storage::Grid& grid,storage::Kind kind,bool compatibleStack,Source source)
    {
        if(!storage::validKind(kind)) return {};
        if(compatibleStack) return {true,true,{}};
        if(const auto placement=grid.firstFit(kind)) return {true,false,placement};
        if(source!=Source::Voluntary) return {true,false,storage::Placement{storage::Location::Overflow}};
        return {};
    }
    // The destination owns admission. Interior capacity is irrelevant to an
    // external mount, for both loose-world deposits and native equipped stows.
    inline Admission forDestination(const storage::Grid& grid,storage::Kind kind,storage::Location location,bool compatibleStack=false)
    {
        if(storage::isMount(location)) {
            const storage::Placement destination{location};
            return grid.fits(kind,destination)==storage::Result::Ok?Admission{true,false,destination}:Admission{};
        }
        return location==storage::Location::Interior?evaluate(grid,kind,compatibleStack,Source::Voluntary):Admission{};
    }
}
