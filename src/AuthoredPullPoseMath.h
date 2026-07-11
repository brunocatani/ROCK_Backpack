#pragma once
#include "PanelPoseMath.h"
#include <ROCK/Abi.h>

namespace rock_backpack::authored_pull
{
    inline panel_math::StoredTransform storedTransform(const rock::api::Transform& value)
    {
        panel_math::StoredTransform result;
        std::copy_n(value.rotate,9,result.rotation.begin());
        result.translation={value.translate[0],value.translate[1],value.translate[2]}; result.scale=value.scale;
        return result;
    }
    // Native row axes: the fixed authored local wrist is reprojected through
    // the current mount, never through the controller used to measure pulling.
    inline rock::api::Transform poseWorld(const rock::api::Transform& parent,const rock::api::Transform& local)
    {
        rock::api::Transform result;
        for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) {
            result.rotate[r*3+c]=0;
            for(unsigned k=0;k<3;++k) result.rotate[r*3+c]+=local.rotate[r*3+k]*parent.rotate[k*3+c];
        }
        const auto point=panel_math::transformLocalPointToWorld(storedTransform(parent),{local.translate[0],local.translate[1],local.translate[2]});
        result.translate[0]=point.x; result.translate[1]=point.y; result.translate[2]=point.z;
        result.scale=parent.scale*local.scale; return result;
    }
}
