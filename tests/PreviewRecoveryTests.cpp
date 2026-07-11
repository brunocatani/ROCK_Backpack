#include "PreviewRecovery.h"
#include <cstdlib>
using namespace rock_backpack::preview_recovery;
static void check(bool ok) { if(!ok) std::abort(); }
int main() {
    Retry retry;
    check(retry.due(0)); retry.failed(100);
    check(!retry.due(1099) && retry.due(1100));
    retry.failed(1100); check(!retry.due(3099) && retry.due(3100));
    for(unsigned i=0;i<20;++i) retry.failed(10000);
    check(retry.failures==6 && retry.after==40000);
    check(!needed(false,false,false,false));
    check(needed(true,false,false,true)); // thumbnail success does not satisfy a mount
    check(!needed(true,false,true,false)); // thumbnail failure cannot invalidate a mount
    check(needed(false,true,true,false));
    check(!needed(true,true,true,true));
    retry={}; check(retry.due(0));
    static_assert(kMaximumProperties>=2*kMaximumNodes);
    std::array<int,97> materials{};
    std::array<void*,kMaximumProperties> properties{};
    for(unsigned i=0;i<materials.size();++i) { properties[2*i]=&materials[i]; properties[2*i+1]=&materials[i]; }
    check(distinctProperties(properties,194)==97); // exceeds the former 32-material ceiling
    check(distinctProperties(properties,0)==0);
    std::array<int,kMaximumProperties> maximum{};
    for(unsigned i=0;i<maximum.size();++i) properties[i]=&maximum[i];
    check(distinctProperties(properties,properties.size())==properties.size());
}
