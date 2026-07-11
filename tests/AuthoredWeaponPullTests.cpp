#include "AuthoredWeaponPull.h"
#include "AuthoredPullPoseMath.h"
#include <cstdlib>
#include <iostream>
using namespace rock_backpack::authored_pull;
void check(bool ok,const char* message) { if(!ok) { std::cerr<<message<<'\n'; std::exit(1); } }
int main()
{
    Input input{true,false,true,{1,4,77,100,20,1,3,4,5},{0,0,0},1,1,3};
    Gesture gesture;
    gesture.update(input);
    check(gesture.update(input)==Phase::Hover,"near an authored pose should hover without drawing");
    input.held=true;
    check(gesture.update(input)==Phase::Latched,"fresh authored grip should latch, not draw");
    input.held=false;
    check(gesture.update(input)==Phase::Cancelled && !gesture.pending,"release without pulling must leave the item stored");
    input.held=true; check(gesture.update(input)==Phase::Latched,"a fresh press should rearm");
    input.controller=input.tracking={5.9f,0,0};
    check(gesture.update(input)==Phase::Latched,"insufficient pull distance must stay locked");
    input.controller=input.tracking={6.1f,0,0};
    check(gesture.update(input)==Phase::Unlock,"controller pull should unlock once the threshold is crossed");
    check(gesture.update(input)==Phase::Unlock,"mailbox contention must retain the pending unlock");
    gesture.acknowledge(); check(gesture.update(input)==Phase::Idle,"held grip cannot repeatedly draw");
    input.held=false; gesture.update(input); input.held=true;
    check(gesture.update(input)==Phase::Latched,"release and new press permit a later pull");
    input.key.reference=101;
    check(gesture.update(input)==Phase::Cancelled,"changed stored reference must cancel the old latch");
    gesture={}; input.held=false; gesture.update(input); input.held=true; gesture.update(input);
    ++input.key.stack;
    check(gesture.update(input)==Phase::Cancelled,"native stack replacement must cancel a latched pull");
    gesture={}; input.held=false; gesture.update(input); input.held=true; gesture.update(input);
    ++input.key.instance;
    check(gesture.update(input)==Phase::Cancelled,"native weapon variant replacement must cancel a latched pull");
    gesture={}; input.held=true;
    check(gesture.update(input)==Phase::Idle && !gesture.latched,"entering with grip already held must not latch");
    input.held=false; gesture.update(input); input.held=true; gesture.update(input);
    input.controller={1000,0,0};
    check(gesture.update(input)==Phase::Cancelled,"a teleport/discontinuity must not unlock a stored item");
    gesture={}; input.controller=input.tracking={0,0,0}; input.held=false; gesture.update(input); input.held=true; gesture.update(input);
    input.key.world++;
    check(gesture.update(input)==Phase::Cancelled,"world transition must cancel authored hand ownership");
    gesture={}; input.held=false; input.scale=2; gesture.update(input); input.held=true; gesture.update(input);
    input.controller={3.1f,0,0}; input.tracking={6.2f,0,0};
    check(gesture.update(input)==Phase::Unlock,"pull threshold is game distance, independent of model scale");
    // Both controller and display moving through the world leave the local
    // controller unchanged: locomotion cannot synthesize a pull.
    gesture={}; input.held=false; gesture.update(input); input.held=true; gesture.update(input);
    check(gesture.update(input)==Phase::Latched,"world motion with unchanged relative controller must stay locked");
    input.controller.x+=10;
    check(gesture.update(input)==Phase::Latched,"moving only the mount must not unlock a weapon");
    input.controller.x-=10; input.tracking.x+=10;
    check(gesture.update(input)==Phase::Latched,"moving mount and controller together must not unlock a weapon");
    rock::api::Transform mount,local;
    local.rotate[0]=local.rotate[4]=local.rotate[8]=1;
    const float rotation[]{0,1,0,-1,0,0,0,0,1};
    std::copy_n(rotation,9,mount.rotate); mount.scale=2;
    mount.translate[0]=100; mount.translate[1]=200; mount.translate[2]=300;
    local.translate[0]=3; local.translate[1]=4; local.translate[2]=5;
    const auto seated=poseWorld(mount,local);
    check(seated.translate[0]==92 && seated.translate[1]==206 && seated.translate[2]==310,"authored hand must follow native row-axis and scale composition");
    for(unsigned i=0;i<9;++i) check(seated.rotate[i]==rotation[i],"fixed local wrist rotation must follow the mount");
}
