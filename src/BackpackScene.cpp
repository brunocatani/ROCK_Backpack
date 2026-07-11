#include "PCH.h"
#include "BackpackScene.h"
#include "CollisionCloneGuard.h"
#include "NativeItemPreview.h"
#include <RE/NetImmerse/NiCloningProcess.h>
#include <RE/NetImmerse/NiCollisionObject.h>
#include <RE/NetImmerse/NiUpdateData.h>
#include <RE/Bethesda/TESDataHandler.h>
#include <cstring>
#include <Windows.h>

namespace rock_backpack
{
    namespace
    {
        panel_math::StoredTransform stored(const RE::NiTransform& value)
        {
            panel_math::StoredTransform out;
            for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) out.rotation[r * 3 + c] = value.rotate.entry[r][c];
            out.translation = {value.translate.x, value.translate.y, value.translate.z}; out.scale = value.scale;
            return out;
        }
        void setLocal(RE::NiAVObject& object, const panel_math::StoredTransform& pose)
        {
            object.local.translate = {pose.translation.x, pose.translation.y, pose.translation.z};
            object.local.scale = pose.scale;
            for (unsigned r = 0; r < 3; ++r) {
                for (unsigned c = 0; c < 3; ++c) object.local.rotate.entry[r][c] = pose.rotation[r * 3 + c];
                object.local.rotate.entry[r][3] = 0;
            }
        }
        // App-culled bit of NiAVObject::flags, written directly as Virtual
        // Holsters does; the virtual SetAppCulled slot is not relied on.
        void setHidden(RE::NiAVObject& object, bool hidden)
        {
            if (hidden) object.flags.flags |= 1ull;
            else object.flags.flags &= ~1ull;
        }
        RE::NiPointer<RE::NiAVObject> clone(RE::NiAVObject* root)
        {
            if (!root) return {};
            RE::NiPointer<RE::NiAVObject> source(root);
            // FO4VR bhkNPCollisionProxyObject vtable 2E83A98 + E0 selects
            // 1C13DA0 (return this). NiAVObject copy 1C22BC1..1C22C0C then
            // calls SetSceneGraphObject on that SAME live proxy. Resetting the
            // clone's collision pointer alone leaves native physics targeting
            // the clone; FindReferenceFor3D can then resolve its player parent.
            // Declare the guard before the result: a rejected clone's destructor
            // can clear shared targets too (1C22F20), so restore after it dies.
            CollisionCloneGuard<RE::NiPointer<RE::NiCollisionObject>,RE::NiPointer<RE::NiAVObject>,1024> bindings;
            std::array<RE::NiAVObject*,1024> sourceNodes{};
            unsigned sourceCount=1, sourceVisited{}; sourceNodes[0]=root;
            bool geometry{};
            for(unsigned i=0;i<sourceCount;++i) {
                auto* node=sourceNodes[i];
                geometry|=node->IsGeometry()!=nullptr;
                const auto collision=node->collisionObject;
                if(collision && !bindings.capture(collision,RE::NiPointer<RE::NiAVObject>(collision->sceneObject))) return {};
                if(auto* branch=node->IsNode()) for(const auto& child:branch->children) {
                    if(++sourceVisited>sourceNodes.size()) return {};
                    if(!child) continue;
                    if(sourceCount==sourceNodes.size()) return {};
                    sourceNodes[sourceCount++]=child.get();
                }
            }
            if(!geometry) return {};
            // Same independently verified FO4VR clone entry used by ROCK's
            // LooseMolotovVisual. Witnesses 1402D9140 and 140723960 construct
            // the 0x78 process and call CreateClone + ProcessClone (1C13FF0).
            constexpr std::array<unsigned char, 15> bytes{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20};
            const auto address = REL::Module::get().base() + 0x1C13FF0;
            if (std::memcmp(reinterpret_cast<const void*>(address), bytes.data(), bytes.size())) return {};
            RE::NiCloningProcess process{};
            process.copyType = RE::NiCloningProcess::CopyType::kCopyExact; process.scale = {1,1,1};
            using Clone = RE::NiObject*(*)(RE::NiObject*, RE::NiCloningProcess*);
            RE::NiPointer<RE::NiObject> object(reinterpret_cast<Clone>(address)(root, &process));
            auto* result = object && object->GetRTTI() == root->GetRTTI() ? static_cast<RE::NiAVObject*>(object.get()) : nullptr;
            if (!result || result == root || result->parent) return {};
            std::array<RE::NiAVObject*, 1024> nodes{}; unsigned count = 1, visited{}; nodes[0] = result;
            for (unsigned i = 0; i < count; ++i) if (auto* node = nodes[i]->IsNode()) for (const auto& child : node->children) {
                if (++visited > nodes.size()) return {};
                if (!child) continue;
                if (count == nodes.size()) return {};
                nodes[count++] = child.get();
            }
            for (unsigned i = 0; i < count; ++i) { nodes[i]->collisionObject.reset(); nodes[i]->controllers.reset(); }
            // bindings restores every source proxy before this detached visual
            // can be attached to the backpack or the player's thigh.
            return RE::NiPointer<RE::NiAVObject>(result);
        }
        void updateDown(RE::NiAVObject& object)
        {
            RE::NiUpdateData data{};
            using Update = void(*)(RE::NiAVObject*, RE::NiUpdateData*);
            // Existing native preview scene-update entry, guarded at plugin load.
            reinterpret_cast<Update>(REL::Module::get().base() + 0x1C22FB0)(&object, &data);
        }
    }
    RE::NiPointer<RE::NiAVObject> BackpackScene::cloneVisual(RE::NiAVObject* source)
    { return clone(source); }
    BackpackScene::Model* BackpackScene::resolveModel(const InventoryDisplayItem& item)
    {
        if(!item) return nullptr;
        for(auto& model:_models) if(model.item.id==item.id && model.item.appearance==item.appearance && model.object) {
            model.item=item; return &model;
        }
        const auto source=item_preview::scene(item.appearance); if(!source) return nullptr;
        // Cache entries are immutable templates. Two exact items can share an
        // appearance after native/logical splitting; a scene node has only one
        // parent, so attaching the shared template makes the other slot vanish.
        for(auto& model:_models) if(!model.object || !model.object->parent) {
            auto visual=cloneVisual(source.get()); if(!visual) return nullptr;
            visual->name=RE::BSFixedString(displayName(item.id).data());
            model={item,std::move(visual)};
            model.contactReady=model.materials.update(model.object.get(),item.form);
            return &model;
        }
        return nullptr;
    }
    bool BackpackScene::inHand(storage::ItemId item) const
    {
        return item && std::any_of(_standIns.begin(),_standIns.end(),[&](const auto& standIn) { return standIn.object && standIn.item==item; });
    }
    void BackpackScene::update(RE::TESObjectREFR& backpack, const std::array<InventoryDisplayItem, 3>& mounts, const zones::Layout& layout,
        const std::array<storage::Kind,3>& kinds, float elapsedSeconds)
    {
        auto* root = backpack.Get3D(); auto* parent = root ? root->IsNode() : nullptr;
        if (!parent || !std::isfinite(root->world.scale) || root->world.scale <= .0001f) { clearBackpack(); return; }
        if (_parent.get() != parent) {
            for (auto& object : _mounted) if (object && object->parent && object->parent==_parent.get()) object->parent->DetachChild(object.get());
            _mounted = {}; _mountEases = {}; _parent.reset(parent);
        }
        const auto now = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < mounts.size(); ++i) {
            // A weapon being drawn stays in the hand until its draw ends, even
            // while the native inventory still lists it on this mount.
            const auto& ref = inHand(mounts[i].id) ? InventoryDisplayItem{} : mounts[i];
            auto* model=ref?resolveModel(ref):nullptr;
            auto* object = model ? model->object.get() : nullptr;
            if (_mounted[i].get() != object || (object && object->parent!=_parent.get())) {
                if (_mounted[i] && _mounted[i]->parent && _mounted[i]->parent==_parent.get()) _mounted[i]->parent->DetachChild(_mounted[i].get());
                _mounted[i].reset(object);
                _mountEases[i] = {};
                if (object) {
                    if(object->parent) object->parent->DetachChild(object);
                    _parent->AttachChild(object, true);
                    setHidden(*object, false);
                    auto& arrival = _arrivals[i];
                    const std::chrono::duration<float> age = now - arrival.at;
                    if (arrival.valid && arrival.form == ref.form && age.count() <= weapon_transition::kArrivalLifetimeSeconds)
                        _mountEases[i].start(arrival.local, weapon_transition::kStashSeconds);
                    arrival = {};
                }
            }
            if (!object || !ref) continue;
            model->contactReady=model->materials.update(object,ref.form);
            panel_math::StoredTransform pose;
            const auto center = zones::mount(storage::Location(i + 1),layout);
            pose.translation = center;
            pose.rotation = zones::visualRotation(storage::Location(i+1),layout,kinds[i]);
            pose.scale = 1.f / root->world.scale;
            setLocal(*object, _mountEases[i].advance(pose, elapsedSeconds));
            updateDown(*object);
        }
    }
    void BackpackScene::expectArrival(storage::Location location, std::uint32_t form, const panel_math::StoredTransform& rootLocal)
    {
        if (!storage::isMount(location) || !form) return;
        _arrivals[unsigned(location)-1] = {form, rootLocal, std::chrono::steady_clock::now(), true};
    }
    bool BackpackScene::beginStandIn(unsigned hand, storage::ItemId item, RE::NiNode& wand, bool followsAim)
    {
        if (hand >= _standIns.size() || !item || !std::isfinite(wand.world.scale) || wand.world.scale <= .0001f) return false;
        endStandIn(hand, false);
        for (unsigned i = 0; i < _mounted.size(); ++i) {
            auto object = _mounted[i];
            if (!object || !_parent || object->parent != _parent.get()) continue;
            const auto model = std::find_if(_models.begin(), _models.end(), [&](const auto& entry) { return entry.object.get() == object.get(); });
            if (model == _models.end() || model->item.id != item) continue;
            const auto local = weapon_transition::localPose(stored(wand.world), stored(object->world));
            if (!local) return false;
            object->parent->DetachChild(object.get());
            wand.AttachChild(object.get(), true);
            setLocal(*object, *local);
            updateDown(*object);
            auto& standIn = _standIns[hand];
            standIn = {object, RE::NiPointer<RE::NiNode>(&wand), item, model->item.form, storage::Location(i + 1), {}, local->scale, followsAim,
                std::chrono::steady_clock::now()};
            if (followsAim) standIn.ease.start(*local, weapon_transition::kDrawSeconds);
            _mounted[i].reset(); _mountEases[i] = {};
            return true;
        }
        return false;
    }
    void BackpackScene::updateStandIn(unsigned hand, const std::optional<panel_math::StoredTransform>& aimLocal, bool hidden, float elapsedSeconds)
    {
        if (hand >= _standIns.size()) return;
        auto& standIn = _standIns[hand];
        auto* object = standIn.object.get();
        if (!object || !standIn.wand || object->parent != standIn.wand.get()) return;
        // Once the real weapon shows, the stand-in stays hidden until removed.
        if (hidden) setHidden(*object, true);
        // Without a fresh aim the stand-in holds its pose; ROCK's aim carries
        // no model scale, so the clone keeps its own.
        if (standIn.followsAim && aimLocal) {
            auto target = *aimLocal;
            target.scale = standIn.scale;
            setLocal(*object, standIn.ease.advance(target, elapsedSeconds));
            updateDown(*object);
        }
    }
    void BackpackScene::endStandIn(unsigned hand, bool returnToMount)
    {
        if (hand >= _standIns.size()) return;
        auto standIn = std::exchange(_standIns[hand], {});
        auto* object = standIn.object.get();
        if (!object) return;
        if (returnToMount && _parent && std::isfinite(_parent->world.scale) && _parent->world.scale > .0001f)
            if (const auto local = weapon_transition::localPose(stored(_parent->world), stored(object->world)))
                _arrivals[unsigned(standIn.location)-1] = {standIn.form, *local, std::chrono::steady_clock::now(), true};
        if (object->parent && object->parent == standIn.wand.get()) object->parent->DetachChild(object);
        setHidden(*object, false);
    }
    void BackpackScene::clearBackpack()
    {
        for(auto& object:_mounted) if(object && object->parent && object->parent==_parent.get()) object->parent->DetachChild(object.get());
        _mounted={}; _mountEases={}; _arrivals={}; _parent.reset();
        // A parked backpack keeps only the clones a hand is still drawing.
        for(auto& model:_models) if(!model.object || !inHand(model.item.id)) model={};
    }
    void BackpackScene::clear(bool sceneAvailable)
    {
        if(sceneAvailable) {
            for(unsigned hand=0;hand<_standIns.size();++hand) endStandIn(hand,false);
            clearBackpack();
        }
        _mounted = {}; _mountEases = {}; _arrivals = {}; _standIns = {}; _models = {}; _parent.reset();
    }
    std::array<BackpackScene::DisplayBinding,3> BackpackScene::displays(std::uint32_t backpack) const
    {
        std::array<DisplayBinding,3> result{};
        const auto ready=[&](RE::NiAVObject* object) -> InventoryDisplayItem {
            if(object) for(const auto& model:_models) if(model.object.get()==object && model.contactReady) return model.item;
            return {};
        };
        for(unsigned i=0;i<_mounted.size();++i) if(_mounted[i] && _parent && _mounted[i]->parent==_parent.get())
            if(const auto ref=ready(_mounted[i].get())) result[i]={backpack,ref.form,i+1u,ref};
        return result;
    }
}
