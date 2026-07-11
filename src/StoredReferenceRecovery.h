#pragma once
#include <cstdint>

namespace rock_backpack::storage_recovery
{
    enum class Result { Retained, Missing, InvalidReference, WrongBase, RetentionFailed };
    struct State { bool loadAttempted{}, retained{}; };

    // Runs on the serialized storage task. The backend loads only the exact
    // native saved reference; there is deliberately no base-form spawn path.
    // A failed native load is attempted once per loaded game. A later cell load
    // may still make that same reference available for retention on the next scan.
    template<class Backend>
    Result restore(std::uint32_t reference, std::uint32_t& expectedBase, State& state, Backend& engine)
    {
        auto ref=engine.lookup(reference);
        if(!ref && !state.loadAttempted) {
            state.loadAttempted=true;
            engine.loadSaved(reference);
            ref=engine.lookup(reference);
        }
        if(!ref) return Result::Missing;
        if(!engine.validWeapon(ref)) return Result::InvalidReference;
        const auto actualBase=engine.base(ref);
        if(!actualBase) return Result::InvalidReference;
        if(expectedBase && actualBase!=expectedBase) return Result::WrongBase;
        if(!engine.retain(ref)) return Result::RetentionFailed;
        // An unmapped light-plugin base ID does not invalidate the exact native
        // saved reference. Its restored base object is the authoritative witness.
        expectedBase=actualBase;
        state.retained=true;
        engine.accept(ref);
        return Result::Retained;
    }
}
