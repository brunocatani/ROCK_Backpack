#pragma once
#include <array>
#include <cstddef>
#include <utility>

namespace rock_backpack
{
    // Native CreateClone can return the original collision proxy, then retarget
    // it to the new scene. Keep the source bindings alive and restore them on
    // every exit, after a rejected clone has run its native destructors.
    template<class CollisionPointer, class ScenePointer, std::size_t Capacity>
    class CollisionCloneGuard
    {
    public:
        CollisionCloneGuard() = default;
        CollisionCloneGuard(const CollisionCloneGuard&) = delete;
        CollisionCloneGuard& operator=(const CollisionCloneGuard&) = delete;
        bool capture(CollisionPointer collision, ScenePointer target)
        {
            if(!collision) return true;
            if(_count==Capacity || collision->sceneObject!=target.get()) return false;
            _bindings[_count++]={std::move(collision),std::move(target)};
            return true;
        }
        ~CollisionCloneGuard() noexcept
        {
            // Directly restore the saved back-pointer. Calling the native
            // setter here could change a source node's collision ownership.
            for(std::size_t i=0;i<_count;++i)
                _bindings[i].collision->sceneObject=_bindings[i].target.get();
        }
    private:
        struct Binding { CollisionPointer collision; ScenePointer target; };
        std::array<Binding,Capacity> _bindings{};
        std::size_t _count{};
    };
}
