#include "BackpackDropPolicy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace rock_backpack;
void require(bool ok,const char* message) { if(!ok) { std::cerr<<message<<'\n'; std::exit(1); } }
int main()
{
    const auto point=drop_policy::nearHand({10010,20030,350},{10000,20000,390},{0,1,-1},{10000,20000,280});
    require(point && point->x==10010 && point->y==20050 && point->z==330,"drop target must be nearby in world units");
    const auto low=drop_policy::nearHand({0,0,5},{0,0,80},{0,0,-1},{0,0,0});
    require(low && low->z==12,"looking down or dropping near feet must not place below player origin");
    require(!drop_policy::nearHand({10000,0,0},{0,0,80},{0,1,0},{0,0,0}),"distant hand snapshot must not remove an item");
    require(!drop_policy::nearHand({0,0,std::numeric_limits<float>::quiet_NaN()},{},{},{0,0,0}),"invalid pose must be rejected");
    using State=drop_policy::Placement;
    require(drop_policy::verify(40,40,{},{},true)==State::Wait,"same-frame relocation cannot confirm completion");
    require(drop_policy::verify(41,40,*point,*point,true)==State::Confirmed,"next-frame correct placement should complete");
    require(drop_policy::verify(41,40,{10010,20500,330},*point,true)==State::Rejected,"old distant physics position must not commit delivery");
    require(drop_policy::verify(41,40,*point,*point,false)==State::Rejected,"wrong-cell placement must not commit delivery");
}
