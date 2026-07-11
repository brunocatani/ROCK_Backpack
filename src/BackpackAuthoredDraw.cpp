#include "PCH.h"
#include "BackpackRuntime.h"
#include "BackpackInteractionPolicy.h"
#include "AuthoredPullPoseMath.h"

namespace rock_backpack
{
    namespace {
        namespace api=rock::api;
        namespace grip=api::storage;
    }
    void BackpackRuntime::clearVisiblePullPresentation(unsigned h,std::uint64_t frame)
    {
        auto& pull=_visiblePulls[h]; const auto hand=h?api::Hand::Left:api::Hand::Right;
        const auto owner=_rock.client.owner();
        const auto clear=[&](bool& owned,api::Status status) {
            if(status==api::Status::Ok || status==api::Status::OwnerNotRegistered) owned=false;
            else if(frame>=_nextVisiblePullDiagnostic) {
                _nextVisiblePullDiagnostic=frame+180;
                logger::warn("Stored authored pull presentation clear deferred: hand={} status={}",h,unsigned(status));
            }
        };
        if(pull.visualOwned && _rock.animation) clear(pull.visualOwned,_rock.animation->clearHandVisualAuthority(owner,hand));
        if(pull.touchOwned && _rock.authoredTouch) clear(pull.touchOwned,_rock.authoredTouch->clearAuthoredHand(owner,hand));
    }
    BackpackRuntime::VisiblePullResult BackpackRuntime::processVisibleWeaponPull(const RockFrame& frame,unsigned h,bool available,bool down,
        const ui::Model* model,const std::array<BackpackScene::DisplayBinding,3>& bindings)
    {
        auto& pull=_visiblePulls[h]; const auto hand=h?api::Hand::Left:api::Hand::Right;
        const auto owner=_rock.client.owner();
        grip::GripPose chosen;
        const storage::Entry* item{}; std::uint32_t parent{};
        if(available && model && model->available && _rock.storage) {
            for(unsigned i=0;i<bindings.size();++i) {
                const auto& binding=bindings[i];
                // Only weapons mounted on the active backpack can be pulled.
                if(!binding.reference || !binding.parent || binding.parent!=_activeBagForm.load()) continue;
                const auto* candidate=model->grid.at(storage::Location(i+1));
                if(!candidate || candidate->busy) continue;
                const auto& peer=_visiblePulls[1-h].gesture;
                if(peer.latched && peer.key.item==candidate->id) continue;
                if(pull.gesture.latched && (binding.id!=pull.gesture.key.display || binding.reference!=pull.gesture.key.reference || candidate->id!=pull.gesture.key.item)) continue;
                grip::GripRequest request;
                request.sample={frame.sample.frameIndex,0,frame.sample.worldGeneration,frame.sample.skeletonGeneration,frame.sample.providerGeneration,0};
                request.displayId=binding.id; request.hand=hand;
                if(pull.gesture.latched) request.role=grip::GripRole(pull.gesture.key.role);
                grip::GripPose pose;
                const auto status=_rock.storage->getAuthoredGrip(owner,&request,&pose);
                if(status!=api::Status::Ok || !pose.valid || pose.item.baseFormId!=binding.reference || pose.item.stackKey!=binding.item.stackWitness || pose.item.instanceKey!=binding.item.instanceWitness || pose.hand!=hand ||
                    pose.sample.frameIndex!=frame.sample.frameIndex || pose.sample.worldGeneration!=frame.sample.worldGeneration ||
                    pose.sample.skeletonGeneration!=frame.sample.skeletonGeneration || pose.sample.providerGeneration!=frame.sample.providerGeneration) continue;
                if(!pull.gesture.latched && pose.distanceGame>pose.acquisitionRadiusGame) continue;
                if(!chosen.valid || pose.distanceGame<chosen.distanceGame) { chosen=pose; item=candidate; parent=binding.parent; }
            }
        }
        authored_pull::Input input;
        input.available=available; input.held=down; input.valid=chosen.valid && item;
        if(input.valid) {
            input.key={item->id,chosen.displayId,chosen.poseSequence,chosen.item.baseFormId,parent,unsigned(chosen.role),frame.sample.worldGeneration,frame.sample.skeletonGeneration,frame.sample.providerGeneration,chosen.item.stackKey,chosen.item.instanceKey};
            input.controller={chosen.controllerInDisplay.x,chosen.controllerInDisplay.y,chosen.controllerInDisplay.z};
            input.tracking={chosen.controllerInTracking.x,chosen.controllerInTracking.y,chosen.controllerInTracking.z};
            input.scale=chosen.displayWorld.scale; input.distance=chosen.distanceGame; input.radius=chosen.acquisitionRadiusGame;
        }
        const bool wasLatched=pull.gesture.latched;
        const auto phase=pull.gesture.update(input);
        if(phase==authored_pull::Phase::Cancelled && wasLatched && down && frame.sample.frameIndex>=_nextVisiblePullDiagnostic) {
            _nextVisiblePullDiagnostic=frame.sample.frameIndex+180;
            logger::info("Stored authored pull cancelled: hand={} pose/identity/tracking changed",h);
        }
        if(phase==authored_pull::Phase::Idle || phase==authored_pull::Phase::Cancelled) {
            clearVisiblePullPresentation(h,frame.sample.frameIndex); return {0,false,false};
        }
        const bool latched=pull.gesture.latched;
        if(latched && !wasLatched) {
            pull.pose=chosen; _pendingEntryCueHands|=1u<<h;
            logger::info("Stored authored pull latched: hand={} display={} ref={:08X} role={}",h,chosen.displayId,chosen.item.baseFormId,unsigned(chosen.role));
        }
        api::touch::AuthoredHand touch;
        touch.sample=chosen.sample; touch.hand=hand; touch.referenceFormId=parent; touch.leaseFrames=3;
        using TouchFlag=api::touch::AuthoredFlag;
        touch.flags=unsigned(TouchFlag::Indicator)|unsigned(TouchFlag::ReserveGrab);
        const auto gripPoint=latched?pull.pose.gripInDisplay:chosen.gripInDisplay;
        const auto contact=panel_math::transformLocalPointToWorld(authored_pull::storedTransform(chosen.displayWorld),{gripPoint.x,gripPoint.y,gripPoint.z});
        touch.contactWorldGame={contact.x,contact.y,contact.z};
        auto status=_rock.authoredTouch->setAuthoredHand(owner,&touch);
        if(status==api::Status::Ok) pull.touchOwned=true;
        if(status==api::Status::Ok && latched) {
            api::animation::HandVisualAuthorityRequest visual;
            visual.hand=hand; visual.flags=unsigned(api::animation::HandVisualAuthorityFlag::WorldTransform)|unsigned(api::animation::HandVisualAuthorityFlag::FingerLocalTransforms);
            visual.priority=100; visual.leaseFrames=3;
            visual.worldGeneration=frame.sample.worldGeneration; visual.skeletonGeneration=frame.sample.skeletonGeneration; visual.providerGeneration=frame.sample.providerGeneration;
            visual.worldTransform=authored_pull::poseWorld(chosen.displayWorld,pull.pose.handInDisplay);
            visual.fingerLocalTransformMask=static_cast<std::uint16_t>(pull.pose.fingerMask);
            std::copy_n(pull.pose.fingerLocals,15,visual.fingerLocalTransforms);
            status=_rock.animation->setHandVisualAuthority(owner,&visual);
            if(status==api::Status::Ok) pull.visualOwned=true;
        }
        if(status!=api::Status::Ok) {
            if(frame.sample.frameIndex>=_nextVisiblePullDiagnostic) {
                _nextVisiblePullDiagnostic=frame.sample.frameIndex+180;
                logger::warn("Stored authored pull presentation refused: hand={} status={}",h,unsigned(status));
            }
            pull.gesture.cancel(); clearVisiblePullPresentation(h,frame.sample.frameIndex); return {0,false,false};
        }
        api::input::HandInputSuppressionRequest suppression;
        suppression.hand=hand; suppression.flags=interaction_policy::zoneInputFlags(false,true); suppression.leaseFrames=3;
        suppression.worldGeneration=frame.sample.worldGeneration; suppression.skeletonGeneration=frame.sample.skeletonGeneration; suppression.providerGeneration=frame.sample.providerGeneration;
        if(_rock.input->setHandInputSuppression(owner,&suppression)!=api::Status::Ok) {
            pull.gesture.cancel(); clearVisiblePullPresentation(h,frame.sample.frameIndex); return {0,false,false};
        }
        if(phase==authored_pull::Phase::Unlock) {
            std::unique_lock lock(_storageMailbox,std::try_to_lock);
            if(lock && !_mountDraw) {
                _mountDraw=MountDraw{_activeBagForm.load(),item->id,model->generation,h?ui::Hand::Left:ui::Hand::Right};
                lock.unlock();
                logger::info("Stored authored pull unlocked: hand={} display={} ref={:08X}",h,chosen.displayId,chosen.item.baseFormId);
                pull.gesture.acknowledge(); clearVisiblePullPresentation(h,frame.sample.frameIndex); scheduleFrameTask();
            }
        }
        return {chosen.displayId,true,latched};
    }
}
