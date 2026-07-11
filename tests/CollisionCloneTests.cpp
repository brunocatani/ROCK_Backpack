#include "CollisionCloneGuard.h"
#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    struct Node;
    struct Collision { Node* sceneObject{}; };
    struct Node {
        std::shared_ptr<Collision> collision;
        Node* parent{};
        bool player{};
        float position{};
        // NiAVObject::~NiAVObject clears the collision target before release.
        ~Node() { if(collision) collision->sceneObject=nullptr; }
    };
    using Guard=rock_backpack::CollisionCloneGuard<std::shared_ptr<Collision>,std::shared_ptr<Node>,4>;
    void check(bool value,const char* message)
    { if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
    auto source(float position)
    {
        auto node=std::make_shared<Node>(); node->position=position;
        node->collision=std::make_shared<Collision>(); node->collision->sceneObject=node.get();
        return node;
    }
    auto nativeClone(const std::shared_ptr<Node>& original, bool sharesProxy)
    {
        auto copy=std::make_shared<Node>(); copy->position=original->position;
        // Native proxy CreateClone returns itself; ordinary collision objects
        // allocate another object. NiAVObject retargets either returned object.
        copy->collision=sharesProxy?original->collision:std::make_shared<Collision>();
        copy->collision->sceneObject=copy.get();
        return copy;
    }
    void nativeSync(const Collision& collision)
    {
        auto* target=collision.sceneObject;
        for(auto* node=target;node;node=node->parent)
            if(node->player) { node->position=target->position; break; }
    }
}
int main()
{
    auto original=source(63.4f);
    Node player; player.player=true;
    auto unsafe=nativeClone(original,true);
    unsafe->collision.reset(); unsafe->parent=&player;
    nativeSync(*original->collision);
    check(player.position==63.4f,"fixture did not reproduce player relocation after collision stripping");
    original->collision->sceneObject=original.get(); player.position=0;
    unsafe.reset();

    std::shared_ptr<Node> display;
    {
        Guard bindings;
        check(bindings.capture(original->collision,original),"valid source binding refused");
        display=nativeClone(original,true);
        display->collision.reset();
    }
    display->parent=&player;
    nativeSync(*original->collision);
    check(player.position==0 && original->collision->sceneObject==original.get(),"shared proxy still resolves the player through its display");
    display.reset();
    check(original->collision->sceneObject==original.get(),"display retirement corrupted source binding");

    {
        Guard bindings;
        bindings.capture(original->collision,original);
        auto rejected=nativeClone(original,true);
        // Failure before sanitization: the clone destructor runs before guard.
    }
    check(original->collision->sceneObject==original.get(),"rejected clone left source proxy cleared");
    try {
        Guard bindings; bindings.capture(original->collision,original);
        auto rejected=nativeClone(original,true);
        throw 1;
    } catch(int) {}
    check(original->collision->sceneObject==original.get(),"clone exception lost source target");

    auto child=source(15);
    {
        Guard bindings;
        bindings.capture(original->collision,original); bindings.capture(child->collision,child);
        auto first=nativeClone(original,false), second=nativeClone(child,true);
        check(original->collision->sceneObject==original.get(),"independent collision copy changed source");
        first->collision.reset(); second->collision.reset();
    }
    check(child->collision->sceneObject==child.get(),"nested shared proxy not restored");
    {
        rock_backpack::CollisionCloneGuard<std::shared_ptr<Collision>,std::shared_ptr<Node>,1> bounded;
        check(bounded.capture(original->collision,original) && !bounded.capture(child->collision,child),"collision census capacity not enforced");
    }
    std::cout<<"Collisionless clones preserve native source ownership on success, rejection and exception.\n";
}
