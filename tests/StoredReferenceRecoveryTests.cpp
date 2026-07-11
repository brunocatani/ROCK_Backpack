#include "StoredReferenceRecovery.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace recovery=rock_backpack::storage_recovery;
void check(bool value,const char* message) { if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
struct Weapon {
    std::uint32_t id{0xFF007894}, base{0x0D00EDD9};
    bool deleted{}, weapon{true}, persistent{};
    std::string modifications{"receiver-A/legendary-B"};
    unsigned rounds{17};
};
struct Engine {
    Weapon saved;
    bool loaded{}, savedAvailable{true}, retentionAllowed{true};
    unsigned loads{}, retains{}, accepts{};
    Weapon* lookup(std::uint32_t id) { return loaded && id==saved.id?&saved:nullptr; }
    void loadSaved(std::uint32_t id) { ++loads; if(id==saved.id && savedAvailable) loaded=true; }
    bool validWeapon(Weapon* ref) { return ref->weapon && !ref->deleted; }
    std::uint32_t base(Weapon* ref) { return ref->base; }
    bool retain(Weapon* ref) { ++retains; return ref->persistent=retentionAllowed; }
    void accept(Weapon*) { ++accepts; }
    void leaveCell() { if(!saved.persistent) loaded=false; }
};
int main()
{
    Engine engine;
    auto base=engine.saved.base;
    recovery::State state;
    check(recovery::restore(engine.saved.id,base,state,engine)==recovery::Result::Retained,"unloaded saved weapon not recovered");
    check(state.loadAttempted && state.retained && engine.loads==1 && engine.accepts==1,"recovery did not load then retain exact reference");
    check(engine.saved.modifications=="receiver-A/legendary-B" && engine.saved.rounds==17,"recovery changed instance state");
    engine.leaveCell();
    check(engine.lookup(engine.saved.id),"retained weapon lost on cell transition");
    state={}; // Death/reload resets runtime-only recovery state, not native item identity.
    check(recovery::restore(engine.saved.id,base,state,engine)==recovery::Result::Retained && !state.loadAttempted,"loaded persisted weapon unnecessarily recreated");

    Engine absent; absent.savedAvailable=false; recovery::State unavailable; auto original=base;
    check(recovery::restore(absent.saved.id,base,unavailable,absent)==recovery::Result::Missing,"missing native item replaced");
    check(recovery::restore(absent.saved.id,base,unavailable,absent)==recovery::Result::Missing && absent.loads==1 &&
        base==original && !unavailable.retained && !absent.accepts,"failure retried native loading or discarded identity");
    absent.loaded=true; // A later real cell load makes the original reference available.
    check(recovery::restore(absent.saved.id,base,unavailable,absent)==recovery::Result::Retained && absent.loads==1,"late native availability could not recover");

    Engine light; light.loaded=true; light.saved.base=0xFE00B59E; std::uint32_t unmapped{}; recovery::State lightState;
    check(recovery::restore(light.saved.id,unmapped,lightState,light)==recovery::Result::Retained && unmapped==0xFE00B59E,
        "failed light-plugin remap masked the exact native weapon");
    recovery::State mismatch; auto wrong=0x01000001u;
    check(recovery::restore(light.saved.id,wrong,mismatch,light)==recovery::Result::WrongBase && wrong==0x01000001 && !mismatch.retained,
        "mismatched resolved base silently rebound");
    light.saved.deleted=true;
    check(recovery::restore(light.saved.id,unmapped,mismatch,light)==recovery::Result::InvalidReference,"deleted native reference resurrected");
    light.saved.deleted=false; light.saved.weapon=false;
    check(recovery::restore(light.saved.id,unmapped,mismatch,light)==recovery::Result::InvalidReference,"non-weapon reference accepted");
    light.saved.weapon=true; light.saved.base=0;
    check(recovery::restore(light.saved.id,unmapped,mismatch,light)==recovery::Result::InvalidReference,"uninitialized base accepted");
    Engine denied; denied.loaded=true; denied.retentionAllowed=false; recovery::State deniedState; auto deniedBase=denied.saved.base;
    check(recovery::restore(denied.saved.id,deniedBase,deniedState,denied)==recovery::Result::RetentionFailed && !deniedState.retained && !denied.accepts,
        "persistence failure reported recovery success");
    std::cout<<"Exact-reference recovery, cell retention, reload, light forms and fail-closed ownership passed.\n";
}
