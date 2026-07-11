#include "PCH.h"
#include "BackpackRuntime.h"
#include "BackpackInteractionPolicy.h"
#include "BackpackDropPolicy.h"
#include <charconv>
#include "f4vr/PlayerNodes.h"

namespace rock_backpack
{
    namespace
    {
        namespace api = rock::api;
        bool holds(const RockFrame& frame, unsigned h, std::uint32_t ref)
        { return ref && (h ? frame.leftHolding && frame.leftHeldFormId == ref : frame.rightHolding && frame.rightHeldFormId == ref); }
        panel_math::StoredTransform transform(const RE::NiTransform& value)
        {
            panel_math::StoredTransform out;
            for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) out.rotation[r * 3 + c] = value.rotate.entry[r][c];
            out.translation = {value.translate.x, value.translate.y, value.translate.z}; out.scale = value.scale; return out;
        }
        api::storage::Target target(std::uint32_t ref, const RockFrame& frame, api::Hand hand = api::Hand::None, std::uint64_t grip = 0)
        {
            api::storage::Target out;
            out.reference = {ref, 0, frame.sample.worldGeneration, frame.sample.skeletonGeneration, frame.sample.providerGeneration};
            out.hand = hand; out.gripSequence = grip; return out;
        }
        std::array<std::optional<panel_math::Vec3>,2> weaponGripPoints()
        {
            std::array<std::optional<panel_math::Vec3>,2> points;
            auto* player=RE::PlayerCharacter::GetSingleton();
            if(!player || !f4cf::f4vr::getFirstPersonSkeleton()) return points;
            // FRIK reparents Weapon/WeaponLeft during ambidextrous switches.
            // Resolve the physical hand from ancestry, never from the node name
            // or configured dominant hand. No scene pointer survives this call.
            const auto* nodes=f4cf::f4vr::getVRPlayerNodes();
            if(!nodes) return points;
            for(const auto* weapon:{f4cf::f4vr::getWeaponNode(),nodes->weaponLeftNode}) {
                if(!weapon) continue;
                const auto p=weapon->world.translate;
                if(!panel_math::isFinite({p.x,p.y,p.z})) continue;
                const auto* parent=weapon->parent;
                for(unsigned depth=0;parent && depth<12;++depth,parent=parent->parent) {
                    const std::string_view name=parent->name.c_str()?parent->name.c_str():"";
                    if(name=="RArm_Hand" || name=="LArm_Hand") {
                        points[name=="LArm_Hand"?1:0]=panel_math::Vec3{p.x,p.y,p.z}; break;
                    }
                }
            }
            return points;
        }
    }
    void BackpackRuntime::resetStorageInteraction() noexcept
    {
        _storageEpoch.fetch_add(1); _activeBagForm = 0; _inventoryOpen = false;
        std::scoped_lock lock(_storageMailbox);
        _weaponDraw.reset(); _weaponDrawResult.reset(); _weaponDrawBusy=false; _inventoryFullHands=0;
        _nativeStowFeedbackHand=ui::Hand::None;
        _stashRequests = {}; _stashAcknowledged = {}; _spatialModel.reset(); _storedReferences.clear(); _mountDraw.reset(); _zoneHint = {}; _mountReferences = {};
        _displayContainer=0;
        _pullHint={}; _collisionDisplays={}; _entryCueHands=0; _standInFeeds={}; _warmMounts={};
    }
    void BackpackRuntime::protectBackpackInput(const RockFrame& frame) noexcept
    {
        if (!_rock.input || !frame.handsValid) return;
        const auto pullingHand = _retrievalBusy.load() ? _retrievalHand.load() : api::Hand::None;
        for (unsigned h = 0; h < 2; ++h) {
            const auto hand = h ? api::Hand::Left : api::Hand::Right;
            const auto held = h ? frame.leftHeldFormId : frame.rightHeldFormId;
            auto* ref = held ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(held) : nullptr;
            const bool bagHeld = ref && ref->GetObjectReference() && ref->GetObjectReference()->formID == _backpackBaseFormId;
            const bool meshSelected = !((h ? frame.leftHolding : frame.rightHolding)) && frame.handStates[h].targetFormId == _activeBagForm.load() && _activeBagForm.load();
            const bool reachReserved=h==1 && _bagPullReserved;
            if (!bagHeld && !meshSelected && pullingHand != hand && !reachReserved) continue;
            api::input::HandInputSuppressionRequest request;
            request.hand = hand; request.leaseFrames = 3;
            request.worldGeneration = frame.sample.worldGeneration; request.skeletonGeneration = frame.sample.skeletonGeneration; request.providerGeneration = frame.sample.providerGeneration;
            // Protect backpack interactions without suppressing the controller's
            // game input: SuppressOpenVrGameInput also zeros locomotion axes.
            request.flags = unsigned(api::input::HandInputSuppressionFlag::SuppressGameplayCandidates) |
                unsigned(api::input::HandInputSuppressionFlag::SuppressHeldWeaponTriggerEquip);
            if (meshSelected) request.flags |= unsigned(api::input::HandInputSuppressionFlag::SuppressNormalGrabPress);
            if (pullingHand == hand || reachReserved) request.flags |= unsigned(api::input::HandInputSuppressionFlag::SuppressNormalGrabPress) |
                unsigned(api::input::HandInputSuppressionFlag::SuppressGrabRelease);
            (void)_rock.input->setHandInputSuppression(_rock.client.owner(), &request);
        }
    }
    void BackpackRuntime::processStorageFrame(const RockFrame& frame) noexcept
    {
        try {
            if (!_rock.storage || !_rock.input || !_rock.grab) return;
            const auto owner = _rock.client.owner();
            const auto settings=_config.snapshot();
            const auto& stashLayout=settings.stash;
            const auto epoch = _storageEpoch.load();
            if (_observedStorageEpoch != epoch) {
                for(unsigned h=0;h<2;++h) { clearVisiblePullPresentation(h,frame.sample.frameIndex); _visiblePulls[h].gesture={}; }
                for (auto& gesture : _zoneGestures) {
                    if (gesture.releaseCommand) (void)_rock.grab->cancelInteractionCommand(owner, gesture.releaseCommand);
                    if (gesture.lease && !gesture.queued) (void)_rock.storage->release(owner, gesture.lease);
                }
                if(_weaponDrawCommand && _rock.weapon) (void)_rock.weapon->cancelInventoryEquip(owner,_weaponDrawCommand);
                _weaponDrawCommand=0; _weaponDrawHand=api::Hand::None; _weaponDrawCompletion.reset();
                if (_bagClaim) (void)_rock.storage->release(owner, _bagClaim);
                _bagClaim = 0; _claimedBag = 0;
                _zoneGestures = {}; _deliveryGestures = {}; _storageClaims = {}; _observedStorageEpoch = epoch; _nextClaimRestoreFrame = 0;
                _pendingEntryCueHands=0; _drawTracking={}; _warm.valid=false;
                for (auto& delivery : _deliveries) delivery = 0;
            }
            std::shared_ptr<const ui::Model> model;
            std::array<std::uint32_t, 256> stored{}; unsigned storedCount{};
            std::array<std::uint64_t, 2> acknowledged{};
            std::array<std::uint64_t,3> mountReferences{};
            std::array<BackpackScene::DisplayBinding,3> collisionDisplays{};
            std::uint32_t displayContainer{};
            {
                std::unique_lock lock(_storageMailbox, std::try_to_lock); if (!lock) return;
                mountReferences = _mountReferences;
                collisionDisplays=_collisionDisplays;
                model = _spatialModel; storedCount = unsigned((std::min)(_storedReferences.size(), stored.size()));
                std::copy_n(_storedReferences.begin(), storedCount, stored.begin());
                acknowledged = std::exchange(_stashAcknowledged, {});
                displayContainer=_displayContainer;
            }
            // Inventory transfer is already complete; the player owns this exact
            // unit while ROCK performs its asynchronous physical-hand equip.
            // Provider commands run only on this callback thread.
            if(_rock.weapon) {
                std::optional<WeaponDraw> request;
                { std::unique_lock lock(_storageMailbox,std::try_to_lock); if(lock && !_weaponDrawCommand) request=std::exchange(_weaponDraw,{}); }
                const auto finish=[&](WeaponDrawResult result) {
                    _weaponDrawCompletion=result;
                    _weaponDrawCommand=0; _weaponDrawHand=api::Hand::None;
                };
                if(request) {
                    auto status=api::Status::NotReady;
                    if(frame.handsValid && frame.sample.worldGeneration==request->world && frame.sample.skeletonGeneration==request->skeleton && frame.sample.providerGeneration==request->provider) {
                        const auto inventory=player_inventory::capture();
                        const auto item=std::find_if(inventory.rows.begin(),inventory.rows.end(),[&](const auto& row) {
                            return row.formId==request->form && row.uniqueBase==request->uniqueBase && row.uniqueIndex==request->uniqueIndex;
                        });
                        if(item!=inventory.rows.end()) {
                            api::weapon::EquipRequest equip; equip.hand=request->hand;
                            status=_rock.weapon->captureInventoryWeapon(owner,item->formId,item->stackId,&equip.item);
                            // A holster draw continues from the stand-in at the
                            // firing aim instead of snapping weapon and hand.
                            if(status==api::Status::Ok) status=request->standIn?
                                _rock.weapon->requestInventoryDraw(owner,&equip,&_weaponDrawCommand):
                                _rock.weapon->requestInventoryEquip(owner,&equip,&_weaponDrawCommand);
                        } else status=api::Status::TargetUnavailable;
                    }
                    const bool queued=status==api::Status::RequestQueued;
                    // A refused draw (command 0) returns its stand-in to the mount.
                    if(request->standIn) _drawTracking[request->hand==api::Hand::Left?1:0]=
                        DrawTracking{queued?_weaponDrawCommand:0,request->form,0,{},true,false};
                    if(queued) _weaponDrawHand=request->hand;
                    else finish({request->hand,false,status});
                }
                if(_weaponDrawCommand) {
                    api::weapon::EquipResult result;
                    const auto status=_rock.weapon->getInventoryEquipResult(owner,_weaponDrawCommand,&result);
                    using State=api::weapon::EquipState;
                    if(status!=api::Status::Ok) finish({_weaponDrawHand,false,status});
                    else if(result.state==State::Succeeded || result.state==State::Failed || result.state==State::Cancelled) {
                        if(result.flags & unsigned(api::weapon::EquipResultFlag::IncomingEquipAccepted))
                            _deliveries[result.hand==api::Hand::Left?1:0]=result.incomingFormId;
                        finish({result.hand,result.state==State::Succeeded,status,result.failure});
                    }
                }
                if(_weaponDrawCompletion) {
                    std::unique_lock lock(_storageMailbox,std::try_to_lock);
                    if(lock) { _weaponDrawResult=std::exchange(_weaponDrawCompletion,{}); lock.unlock(); scheduleFrameTask(); }
                }
            }
            trackDrawStandIns(frame);
            serviceWarmMounts(frame);
            for (unsigned h = 0; h < 2; ++h) if (_zoneGestures[h].queued && acknowledged[h] && acknowledged[h] == (_zoneGestures[h].lease?_zoneGestures[h].lease:UINT64_MAX)) {
                const auto down = _zoneGestures[h].down;
                _zoneGestures[h] = {}; _zoneGestures[h].valid = true; _zoneGestures[h].down = down;
            }
            const bool ready = _sessionActive.load() && _backpackFeatureEnabled.load() && frame.providerReady && frame.handsValid && frame.headValid &&
                !frame.menuBlocking && !frame.configBlocking;
            if(_rock.storage) {
                if(!ready || !model || !model->available || !_activeBagForm.load()) {
                    (void)_rock.storage->clearDisplaysAndRouting(owner);
                }
                else {
                    // Nothing stashes over the shoulders: ROCK's built-in
                    // shoulder stash stays off while the backpack is in use,
                    // and the left shoulder only takes the backpack off.
                    api::storage::ShoulderRouting routing;
                    routing.handMask=3; routing.worldGeneration=frame.sample.worldGeneration;
                    routing.skeletonGeneration=frame.sample.skeletonGeneration; routing.providerGeneration=frame.sample.providerGeneration;
                    const auto routingStatus=_rock.storage->setShoulderRouting(owner,&routing);
                    if(routingStatus!=api::Status::Ok && frame.sample.frameIndex>=_nextInteractionDiagnostic) {
                        _nextInteractionDiagnostic=frame.sample.frameIndex+180;
                        logger::warn("ROCK shoulder stash suppression refused, status={}",unsigned(routingStatus));
                    }
                    for(unsigned i=0;i<collisionDisplays.size();++i) {
                        const auto& binding=collisionDisplays[i];
                        const auto& item=binding.item;
                        api::storage::Display request;
                        auto status=api::Status::TargetUnavailable;
                        if(binding.parent && item && _rock.weapon) {
                            status=_rock.weapon->captureInventoryWeapon(owner,item.form,item.index,&request.item);
                            if(status==api::Status::Ok && (request.item.stackKey!=item.stackWitness || request.item.instanceKey!=item.instanceWitness || request.item.count!=item.count))
                                status=api::Status::TargetUnavailable;
                            if(status==api::Status::Ok) {
                                request.displayId=binding.id; request.parentReference=binding.parent;
                                const auto name=BackpackScene::displayName(item.id); std::copy(name.begin(),name.end(),request.nodeName);
                                request.flags|=static_cast<std::uint32_t>(api::storage::DisplayFlag::NoCollision);
                                status=_rock.storage->setDisplay(owner,&request);
                            }
                        }
                        if(status!=api::Status::Ok) (void)_rock.storage->clearDisplay(owner,i+1);
                    }
                }
            }
            // A Native storage commit remains protected while its copied task
            // request is in flight; afterward the saved ledger is authoritative.
            for (auto& claim : _storageClaims) if (claim.lease) {
                bool wanted = std::find(stored.begin(), stored.begin() + storedCount, claim.reference) != stored.begin() + storedCount;
                for (const auto& gesture : _zoneGestures) wanted |= gesture.lease == claim.lease && (gesture.queued || gesture.releasing || gesture.reference);
                if (!wanted) {
                    const auto status = _rock.storage->release(owner, claim.lease);
                    if (status == api::Status::Ok || status == api::Status::RequestNotFound) claim = {};
                }
            }
            if (ready && storedCount && frame.sample.frameIndex >= _nextClaimRestoreFrame) {
                _nextClaimRestoreFrame = frame.sample.frameIndex + 90;
                for (unsigned i = 0; i < storedCount; ++i) {
                    if (holds(frame, 0, stored[i]) || holds(frame, 1, stored[i])) continue;
                    if (std::any_of(_storageClaims.begin(), _storageClaims.end(), [&](const auto& claim) { return claim.reference == stored[i]; })) continue;
                    auto slot = std::find_if(_storageClaims.begin(), _storageClaims.end(), [](const auto& claim) { return !claim.lease; });
                    if (slot == _storageClaims.end()) break;
                    auto request = target(stored[i], frame); std::uint64_t lease{};
                    if (_rock.storage->claim(owner, &request, &lease) == api::Status::Ok) {
                        if (_rock.storage->commit(owner, lease) == api::Status::Ok) *slot = {stored[i], lease};
                        else (void)_rock.storage->release(owner, lease);
                    }
                }
            }
            const auto bagId = _activeBagForm.load();
            auto* bag = ready && bagId ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(bagId) : nullptr;
            const auto* root = bag && !bag->IsDisabled() && !bag->IsDeleted() ? bag->Get3D() : nullptr;
            const bool active = root && model && model->available;
            const auto grips=active?weaponGripPoints():decltype(weaponGripPoints()){};
            if (_bagClaim && (!active || _claimedBag != bagId)) {
                (void)_rock.storage->release(owner,_bagClaim); _bagClaim=0; _claimedBag=0;
            }
            ZoneHint hint; hint.frame = frame.sample.frameIndex;
            if (model) for (unsigned slot=1;slot<=3;++slot) if (model->grid.at(storage::Location(slot)))
                hint.guidance.blocked |= 1u << slot;
            if (!active) (void)_rock.storage->clearHeldContacts(owner);
            for (unsigned h = 0; h < 2; ++h) {
                const auto hand = h ? api::Hand::Left : api::Hand::Right;
                struct FeedbackScope {
                    const api::storage::Api* api; api::OwnerToken owner; api::Hand hand; bool keep{};
                    ~FeedbackScope() { if (api && !keep) (void)api->clearCandidate(owner, hand); }
                } feedback{_rock.storage, owner, hand};
                struct DisplayScope {
                    const api::storage::Api* api; api::OwnerToken owner; api::Hand hand; bool keep{};
                    ~DisplayScope() { if (api && !keep) (void)api->clearSelection(owner,hand); }
                } display{_rock.storage,owner,hand};
                const auto& state = frame.handStates[h]; auto& gesture = _zoneGestures[h];
                const auto suppress = [&](bool deposit, bool mounted) {
                    api::input::HandInputSuppressionRequest request;
                    request.hand=hand; request.leaseFrames=3;
                    request.worldGeneration=frame.sample.worldGeneration; request.skeletonGeneration=frame.sample.skeletonGeneration; request.providerGeneration=frame.sample.providerGeneration;
                    request.flags=interaction_policy::zoneInputFlags(deposit,mounted);
                    return _rock.input->setHandInputSuppression(owner,&request);
                };
                api::input::RawWandButtonState button{};
                const bool available = ready && _rock.input->getRawWandButtonState(owner, hand, 2, &button) == api::Status::Ok && button.available && button.sampleAgeMilliseconds < 150;
                const bool down = available && button.held;
                const bool pressed = available && gesture.valid && !gesture.down && down;
                const bool released = available && gesture.valid && gesture.down && !down;
                gesture.valid = available; gesture.down = down;
                // Retrieval is resolved first and owns the complete pull,
                // including the exit frame and the asynchronous hand delivery.
                const bool bagPullZone=h==1 && _bagPullReserved;
                api::storage::Contact contact;
                const bool meshContact=ready && _rock.storage && _rock.storage->getDisplayContact(owner,hand,&contact)==api::Status::Ok && contact.valid &&
                    contact.sample.worldGeneration==frame.sample.worldGeneration && contact.sample.skeletonGeneration==frame.sample.skeletonGeneration && contact.sample.providerGeneration==frame.sample.providerGeneration &&
                    contact.displayId>=1 && contact.displayId<=collisionDisplays.size() && collisionDisplays[contact.displayId-1].item.stackWitness==contact.item.stackKey && collisionDisplays[contact.displayId-1].item.instanceWitness==contact.item.instanceKey;
                const bool pullAvailable=available && !bagPullZone && !(h?frame.leftHolding:frame.rightHolding) && !_retrievalBusy.load() &&
                    !_weaponDrawBusy.load() && !gesture.releasing && !gesture.queued;
                // A mounted weapon is drawn only by gripping it on the backpack
                // and pulling it off (an authored pull).
                if(processVisibleWeaponPull(frame,h,pullAvailable,down,model.get(),collisionDisplays).ownsInput) continue;
                if (const auto delivery = _deliveries[h].exchange(0)) _deliveryGestures[h].delivered(delivery);
                const auto currentHeld = h ? frame.leftHeldFormId : frame.rightHeldFormId;
                const bool depositGesture = _deliveryGestures[h].permits(currentHeld,available,down,pressed);
                if (active && holds(frame, h, bagId)) {
                    auto request = target(bagId, frame, hand, state.gripSequence);
                    (void)_rock.storage->suppressHeldContacts(owner, &request);
                    // A transient owner claim prevents ordinary mesh grabs and
                    // hand-to-hand theft while the backpack is being used.
                    if (_rock.storage->claim(owner, &request, &_bagClaim) == api::Status::Ok) _claimedBag=bagId;
                }
                if (gesture.queued || (_weaponDrawBusy.load() && hand==_weaponDrawHand)) continue;
                if (gesture.releasing) {
                    if (!ready || gesture.bag != bagId || frame.sample.frameIndex > gesture.deadline) {
                        if (gesture.releaseCommand) (void)_rock.grab->cancelInteractionCommand(owner, gesture.releaseCommand);
                        if (gesture.lease) (void)_rock.storage->release(owner, gesture.lease);
                        gesture = {}; continue;
                    }
                    (void)suppress(true,false);
                    if (gesture.releaseCommand) {
                        api::grab::InteractionCommandResult result;
                        if (_rock.grab->getInteractionCommandResult(owner, gesture.releaseCommand, &result) != api::Status::Ok ||
                            result.state == api::grab::InteractionCommandState::Queued) continue;
                        if (result.state != api::grab::InteractionCommandState::Succeeded) {
                            (void)_rock.storage->release(owner, gesture.lease); gesture = {}; continue;
                        }
                        gesture.releaseCommand = 0;
                    }
                    if (holds(frame, 0, gesture.reference) || holds(frame, 1, gesture.reference)) continue;
                    if (!gesture.lease) {
                        auto request = target(gesture.reference, frame);
                        if (_rock.storage->claim(owner, &request, &gesture.lease) != api::Status::Ok) continue;
                    }
                    const auto committed = _rock.storage->commit(owner, gesture.lease);
                    if (committed == api::Status::Busy || committed == api::Status::ObjectAlreadyOwned || committed == api::Status::NotReady) continue;
                    if (committed != api::Status::Ok) { (void)_rock.storage->release(owner, gesture.lease); gesture = {}; continue; }
                    auto slot = std::find_if(_storageClaims.begin(), _storageClaims.end(), [&](const auto& claim) { return !claim.lease || claim.lease == gesture.lease; });
                    if (slot == _storageClaims.end()) { (void)_rock.storage->release(owner, gesture.lease); gesture = {}; continue; }
                    // The released weapon's last visible pose starts its mount ease.
                    const auto arrival=root && storage::isMount(gesture.location)?heldWeaponArrival(frame,h,gesture.reference,*root):std::nullopt;
                    std::unique_lock lock(_storageMailbox, std::try_to_lock); if (!lock || _stashRequests[h]) continue;
                    *slot = {gesture.reference, gesture.lease};
                    _stashRequests[h] = StashRequest{gesture.reference, bagId, frame.sample.worldGeneration, frame.sample.skeletonGeneration, frame.sample.providerGeneration,
                        gesture.location, hand, gesture.lease, false, gesture.form, arrival};
                    gesture.queued = true; lock.unlock();
                    scheduleFrameTask(); continue;
                }
                if (!active || !available || holds(frame,h,bagId)) {
                    if (gesture.lease) (void)_rock.storage->release(owner,gesture.lease);
                    gesture.reference=gesture.equipped=0; gesture.lease=0; gesture.hover.reset();
                    (void)_rock.input->clearHandInputSuppression(owner,hand); continue;
                }
                const bool occupied = h ? frame.leftHolding : frame.rightHolding;
                constexpr auto looseFlags = unsigned(api::grab::HandInteractionFlag::LooseObject) | unsigned(api::grab::HandInteractionFlag::LooseWeapon);
                const auto loose = occupied && state.targetFormId != bagId && (state.flags & looseFlags) && !holds(frame, 1 - h, state.targetFormId) ? state.targetFormId : 0;
                const bool nativeCarry = occupied && !loose && interaction_policy::carriesEquipped(state.flags, state.targetFormId, frame.equippedWeapon,gesture.equipped);
                if (loose && loose != gesture.reference) {
                    if (gesture.lease) (void)_rock.storage->release(owner, gesture.lease);
                    gesture.reference = gesture.equipped = 0; gesture.lease = 0;
                    if (auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(loose)) {
                        const auto value = player_inventory::describe(*ref);
                        if (value.formId && value.count) { gesture.reference = loose; gesture.kind = value.kind; gesture.form = value.formId; }
                    }
                }
                if (nativeCarry) {
                    if (gesture.reference) { if (gesture.lease) (void)_rock.storage->release(owner, gesture.lease); gesture.reference = 0; gesture.lease = 0; }
                    gesture.equipped = 0;
                    api::weapon::Classification classification;
                    if (_rock.weapon->getClassification(owner, 0, &classification) == api::Status::Ok && classification.valid && classification.weaponFormId == frame.equippedWeapon) {
                        gesture.equipped = frame.equippedWeapon;
                        const auto* weapon=RE::TESForm::GetFormByID<RE::TESObjectWEAP>(classification.weaponFormId);
                        using Size=api::weapon::SizeClass;
                        using Flag=api::weapon::KeywordFlag;
                        const auto flags=classification.keywordFlags;
                        gesture.kind=storage::weaponKind(classification.sizeClass==Size::Melee,
                            weapon && unsigned(*weapon->weaponData.type)==2,classification.sizeClass==Size::Pistol,
                            (flags & std::uint64_t(Flag::Sniper))!=0,classification.sizeClass==Size::Heavy,
                            (flags & (std::uint64_t(Flag::Thrown)|std::uint64_t(Flag::Grenade)|std::uint64_t(Flag::Mine)))!=0);
                    }
                }
                if (!nativeCarry && !released) gesture.equipped = 0;
                if ((!occupied && !down && !released) || (gesture.reference && holds(frame, 1 - h, gesture.reference))) {
                    if (gesture.lease) (void)_rock.storage->release(owner, gesture.lease);
                    gesture.reference = gesture.equipped = 0; gesture.lease = 0;
                }
                const bool depositing = occupied || gesture.reference || gesture.equipped;
                // Backpack stashing uses the physical hand's Weapon/WeaponLeft
                // grip node for both equipped and loose items. The hand frame
                // is at the wrist and must not replace this interaction point.
                const auto local = grips[h] ? zones::localPoint(transform(root->world),*grips[h]):std::nullopt;
                const auto allowed = depositing ? zones::depositMask(gesture.kind) : zones::drawMask(model->grid);
                if (gesture.reference || gesture.equipped) hint.guidance.visible |= allowed;
                const auto region = depositing && local ? zones::choose(*local,true,allowed,gesture.hover,stashLayout) :
                    !depositing && !bagPullZone && meshContact && collisionDisplays[contact.displayId-1].parent==bagId?std::optional{storage::Location(contact.displayId)}:std::nullopt;
                gesture.hover=region;
                if(!grips[h] && frame.sample.frameIndex>=gesture.nextDiagnostic) {
                    gesture.nextDiagnostic=frame.sample.frameIndex+180;
                    logger::warn("Backpack weapon grip node unavailable for hand {}; zone detection withheld",h);
                }
                if (occupied && local && frame.sample.frameIndex >= gesture.nextDiagnostic) {
                    gesture.nextDiagnostic=frame.sample.frameIndex+180;
                    logger::info("Backpack zone probe: hand={} held={:08X} flags={:X} loose={:08X} equipped={:08X} kind={} weaponNodeLocal=({:.2f},{:.2f},{:.2f}) zone={} grabDown={} release={} deliveryReady={}",
                        h,currentHeld,state.flags,gesture.reference,gesture.equipped,unsigned(gesture.kind),local->x,local->y,local->z,
                        region?int(*region):-1,down,released,depositGesture);
                }
                if (!region) {
                    if (gesture.lease) (void)_rock.storage->release(owner, gesture.lease);
                    gesture.lease = 0; // Keep classification for this same held item outside the volume.
                    (void)_rock.input->clearHandInputSuppression(owner, hand); continue;
                }
                const bool candidate = gesture.reference || gesture.equipped;
                const bool possibleStack = gesture.reference && std::any_of(model->grid.spatialIndices().begin(), model->grid.spatialIndices().end(), [&](const auto index) {
                    const auto& entry = model->grid.entries()[index];
                    const auto item=model->itemByStorageId.find(entry.id);
                    return entry.stackKey && !entry.busy && item!=model->itemByStorageId.end() && model->items[item->second].formId==gesture.form;
                });
                // Exact native stack compatibility is revalidated by the task
                // before mutation. A full board can still accept an existing stack.
                const bool fits = candidate && inventory_capacity::forDestination(model->grid,gesture.kind,*region,
                    !gesture.equipped && possibleStack).accepted;
                const auto* mounted = !candidate && *region != storage::Location::Interior ? model->grid.at(*region) : nullptr;
                bool accepted = fits;
                if (loose && fits) {
                    auto request=target(loose,frame,hand,state.gripSequence);
                    if (_rock.storage->claim(owner,&request,&gesture.lease)!=api::Status::Ok) accepted=false;
                }
                // Reserve both gameplay edges before ROCK handles its toggle or
                // auto-drop. The raw button levels above remain addon-owned.
                const auto suppressionStatus=suppress(candidate,mounted!=nullptr);
                accepted = accepted && suppressionStatus == api::Status::Ok;
                if(candidate && !fits && released && depositGesture && suppressionStatus==api::Status::Ok) {
                    _inventoryFullHands.fetch_or(1u<<h); scheduleFrameTask();
                }
                if (candidate && !accepted) hint.guidance.blocked |= 1u << unsigned(*region);
                if (suppressionStatus == api::Status::Ok && mounted && !mounted->busy && !occupied && !pressed && !_retrievalBusy.load() && _rock.storage) {
                    const auto reference=mountReferences[unsigned(*region)-1];
                    if (reference) {
                        api::storage::Selection request;
                        request.hand=hand; request.parentReference=bagId; request.storedReference=displayContainer;
                        request.worldGeneration=frame.sample.worldGeneration; request.skeletonGeneration=frame.sample.skeletonGeneration; request.providerGeneration=frame.sample.providerGeneration;
                        const auto name=BackpackScene::displayName(reference); std::copy(name.begin(),name.end(),request.nodeName);
                        const auto status=_rock.storage->setSelection(owner,&request);
                        display.keep=status==api::Status::Ok || status==api::Status::NotReady;
                    }
                }
                if (candidate) {
                    if (gesture.bag != bagId || gesture.location != *region)
                        logger::info("Backpack physical storage candidate: hand={}, reference={:08X}, equipped={:08X}, zone={}, kind={}, accepted={}",
                            h,gesture.reference,gesture.equipped,unsigned(*region),unsigned(gesture.kind),fits);
                    gesture.bag = bagId; gesture.location = *region;
                    const auto decision=interaction_policy::depositDecision(fits && accepted,released,depositGesture);
                    if (decision.feedback) {
                        hint.guidance.ready |= 1u << unsigned(*region);
                        hint.readyHands |= 1u << h;
                    }
                    if (decision.feedback && occupied && _rock.storage) {
                        api::storage::Candidate request;
                        request.hand = hand; request.reference = gesture.reference;
                        request.expectedBase = gesture.equipped ? gesture.equipped : gesture.form;
                        request.worldGeneration = frame.sample.worldGeneration; request.skeletonGeneration = frame.sample.skeletonGeneration;
                        request.providerGeneration = frame.sample.providerGeneration; request.gripSequence = state.gripSequence;
                        const auto status = _rock.storage->setCandidate(owner, &request);
                        feedback.keep = status == api::Status::Ok || status == api::Status::NotReady;
                        if (status != api::Status::Ok && status != api::Status::NotReady && (gesture.nextDiagnostic == frame.sample.frameIndex+180))
                            logger::warn("Backpack stash visual refused: hand={} ref={:08X} equipped={:08X} status={}",h,gesture.reference,gesture.equipped,unsigned(status));
                    }
                    if (!decision.commit) continue;
                    gesture.deadline = frame.sample.frameIndex + 180;
                    if (gesture.equipped) {
                        // The equipped weapon is still in the hand this frame;
                        // its pose starts the mount visual's ease.
                        const auto arrival=storage::isMount(gesture.location)?heldWeaponArrival(frame,h,0,*root):std::nullopt;
                        std::unique_lock lock(_storageMailbox,std::try_to_lock);
                        if(!lock || _stashRequests[h]) continue;
                        _stashRequests[h]=StashRequest{gesture.equipped,bagId,frame.sample.worldGeneration,frame.sample.skeletonGeneration,
                            frame.sample.providerGeneration,gesture.location,hand,0,true,gesture.equipped,arrival};
                        gesture.queued=true; lock.unlock(); scheduleFrameTask(); continue;
                    } else if (!gesture.lease) continue;
                    else if (holds(frame, h, gesture.reference)) {
                        api::grab::ForceReleaseRequest request;
                        request.hand = hand; request.targetFormId = gesture.reference;
                        request.worldGeneration = frame.sample.worldGeneration; request.skeletonGeneration = frame.sample.skeletonGeneration; request.providerGeneration = frame.sample.providerGeneration;
                        if (_rock.grab->requestForceRelease(owner, &request, &gesture.releaseCommand) != api::Status::RequestQueued) continue;
                    }
                    gesture.releasing = true;
                }
            }
            if (!hint.guidance.visible) hint.guidance.visible=15;
            std::unique_lock lock(_storageMailbox, std::try_to_lock);
            if (lock) {
                _zoneHint = hint;
                const auto entered=std::exchange(_pendingEntryCueHands,0);
                lock.unlock();
                if(entered) { _entryCueHands.fetch_or(entered); scheduleFrameTask(); }
            }
        } catch (...) { logger::error("Backpack storage frame failed; no native inventory mutation was attempted on this boundary"); }
    }
    void BackpackRuntime::publishCollisionDisplays() noexcept
    {
        const auto bindings=_scene.displays(_activeBagForm.load());
        std::scoped_lock lock(_storageMailbox); _collisionDisplays=bindings;
    }
    void BackpackRuntime::servicePullPrompt() noexcept
    {
        PullHint pull;
        { std::scoped_lock lock(_storageMailbox); pull=_pullHint; }
        api::core::Snapshot current;
        const bool sampled=_rock.client.core() && _rock.client.core()->getSnapshot(_rock.client.owner(),&current)==api::Status::Ok;
        const bool fresh=sampled && current.providerReady && current.frikSkeletonReady && !current.menuBlocking && !current.configBlocking &&
            _sessionActive.load() && _backpackFeatureEnabled.load() && _backpackOnBack.load() && pull.visible &&
            pull.bag==_activeBagForm.load() && pull.world==current.sample.worldGeneration && pull.skeleton==current.sample.skeletonGeneration &&
            pull.provider==current.sample.providerGeneration && current.sample.frameIndex>=pull.frame && current.sample.frameIndex-pull.frame<=3;
        const auto pose=fresh?panel_math::pullPromptPose(pull.eye,pull.forward,pull.right):std::nullopt;
        if(pose && _pullMarker.ensureRegistered(18,14.4f)) {
            _pullMarker.publishPullPrompt(pull.prompt); (void)_pullMarker.present(*pose,18,14.4f);
        } else _pullMarker.hide();
        publishCollisionDisplays();
    }

    void BackpackRuntime::serviceStorageRequests()
    {
        if(const auto full=_inventoryFullHands.exchange(0)) {
            _opener.notifyInventoryFull();
            for(unsigned h=0;h<2;++h) if(full&(1u<<h)) queueHaptic(h?ui::Hand::Left:ui::Hand::Right,HapticCue::Rejected);
        }
        std::optional<WeaponDrawResult> equipped;
        { std::scoped_lock lock(_storageMailbox); equipped=std::exchange(_weaponDrawResult,{}); }
        if(equipped) {
            _weaponDrawBusy=false; _actionHand=equipped->hand==api::Hand::Left?ui::Hand::Left:ui::Hand::Right;
            logger::info("Backpack hand equip completed: hand={} success={} status={} failure={}", unsigned(equipped->hand),equipped->success,unsigned(equipped->status),unsigned(equipped->failure));
            completeItemAction(equipped->success?"Weapon equipped in the drawing hand":"Equip failed; the exact weapon remains in player inventory",equipped->success);
        }
        auto& storage = BackpackStorage::get();
        api::core::Snapshot weightSnapshot;
        if (_rock.client.core() && _rock.client.owner() &&
            _rock.client.core()->getSnapshot(_rock.client.owner(), &weightSnapshot) == api::Status::Ok) {
            const auto weightFrame = _rock.sample(weightSnapshot);
            storage.refreshWeight(weightFrame.rightHolding ? weightFrame.rightHeldFormId : 0, weightFrame.leftHolding ? weightFrame.leftHeldFormId : 0);
        }
        if (storage.pending() && !_retrievalBusy.load()) {
            api::core::Snapshot core;
            if (_rock.client.core()->getSnapshot(_rock.client.owner(), &core) == api::Status::Ok) {
                const auto frame = _rock.sample(core);
                const auto ref = storage.pendingReference().get();
                if (frame.handsValid && ref) {
                    if (holds(frame, 0, ref->formID) || holds(frame, 1, ref->formID)) (void)storage.completeDelivery(true);
                    else if (storage.needsRecovery()) (void)storage.completeDelivery(false);
                }
            }
        }
        std::array<std::optional<StashRequest>, 2> requests;
        std::optional<MountDraw> draw;
        { std::scoped_lock lock(_storageMailbox); requests = std::exchange(_stashRequests, {}); draw = std::exchange(_mountDraw, {}); }
        for (unsigned h = 0; h < requests.size(); ++h) if (const auto& request = requests[h]) {
            bool stored{}, capacityRejected{};
            api::core::Snapshot core;
            if (_rock.client.core()->getSnapshot(_rock.client.owner(), &core) == api::Status::Ok) {
                const auto frame = _rock.sample(core);
                if(request->nativeEquipped && _activeBagForm.load()==request->bag && frame.handsValid && !frame.menuBlocking &&
                    frame.sample.worldGeneration==request->world && frame.sample.skeletonGeneration==request->skeleton && frame.sample.providerGeneration==request->provider)
                    stored=storage.stowEquipped(request->reference,request->location);
                auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(request->reference);
                if (!request->nativeEquipped && _activeBagForm.load() == request->bag && frame.handsValid && !frame.menuBlocking &&
                    frame.sample.worldGeneration == request->world && frame.sample.skeletonGeneration == request->skeleton && frame.sample.providerGeneration == request->provider &&
                    !holds(frame, 0, request->reference) && !holds(frame, 1, request->reference) && ref && !ref->IsDisabled() && !ref->IsDeleted())
                    stored = storage.deposit(*ref, request->location,&capacityRejected);
            }
            if(stored && request->arrival) _scene.expectArrival(request->location,request->form,*request->arrival);
            _inventoryStatus = stored ? (storage.stowPending()?"Assigning weapon to slot":"Item stored") : "Storage refused; the item remains with its native owner";
            if(capacityRejected) _opener.notifyInventoryFull();
            if(stored && (request->nativeEquipped || storage.stowPending())) _nativeStowFeedbackHand=h?ui::Hand::Left:ui::Hand::Right;
            else queueHaptic(h ? ui::Hand::Left : ui::Hand::Right, stored ? HapticCue::ItemCommitted : HapticCue::Rejected);
            sendInventoryModel();
            { std::scoped_lock lock(_storageMailbox); _stashAcknowledged[h] = request->nativeEquipped?UINT64_MAX:request->lease; }
        }
        if(const auto result=storage.takeStowResult()) {
            _inventoryStatus=*result?"Weapon stored":"Holster refused; weapon remains in player inventory";
            if(_nativeStowFeedbackHand!=ui::Hand::None) queueHaptic(_nativeStowFeedbackHand,*result?HapticCue::ItemCommitted:HapticCue::Rejected);
            _nativeStowFeedbackHand=ui::Hand::None; sendInventoryModel();
        }
        if (draw && draw->bag == _activeBagForm.load()) {
            ui::Action action{.kind = ui::ActionKind::Grab, .generation = draw->generation, .hand = draw->hand};
            action.exactHand = true;
            std::snprintf(action.rowId.data(), action.rowId.size(), "%llu", static_cast<unsigned long long>(draw->item));
            handleStorageAction(action);
        }
    }
    void BackpackRuntime::handleStorageAction(const ui::Action& action)
    {
        auto& storage = BackpackStorage::get();
        _actionHand = action.hand;
        if (action.kind == ui::ActionKind::Open) { _inventoryOpen = !_inventoryOpen; return; }
        if (action.kind == ui::ActionKind::Close) { _inventoryOpen = false; _panel.hide(); _panelVisible = false; return; }
        if (action.generation != storage.generation()) { completeItemAction("Inventory changed; select the item again", false); return; }
        if (action.kind == ui::ActionKind::Move || action.kind == ui::ActionKind::Split || action.kind == ui::ActionKind::Merge) {
            const bool moved = storage.arrange(action) == storage::Result::Ok;
            completeItemAction(moved ? "Inventory organized" : "That placement or stack operation is not valid", moved); return;
        }
        storage::ItemId id{};
        const auto end = action.rowId.data() + strnlen(action.rowId.data(), action.rowId.size());
        const auto parsed = std::from_chars(action.rowId.data(), end, id);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !id) return;
        if (action.kind == ui::ActionKind::Use || action.kind == ui::ActionKind::EquipArmor || action.kind == ui::ActionKind::UnequipArmor || action.kind == ui::ActionKind::ToggleWeapon) {
            const bool used = storage.use(id, action.kind,action.generation);
            completeItemAction(used ? "Item action accepted" : "The item could not be used or equipped", used); return;
        }
        if (_retrievalBusy.load() || storage.pending() || _weaponDrawBusy.load()) { completeItemAction("An item transfer is in progress", false); return; }
        const auto destination = resolveFreeHandDelivery();
        if (!destination.valid) { completeItemAction("The pointing hand must be free to receive this item", false); return; }
        if (action.hand != ui::Hand::None && destination.hand != (action.hand == ui::Hand::Left ? api::Hand::Left : api::Hand::Right)) {
            completeItemAction("The hand drawing this item must be free", false); return;
        }
        const auto kind = storage.kind(id);
        if ((action.kind == ui::ActionKind::Grab || action.kind == ui::ActionKind::ToggleWeapon) && kind && interaction_policy::equipsOnDraw(*kind)) {
            if(!_rock.weapon) { completeItemAction("ROCK Weapon is required for hand-specific equip",false); return; }
            // A weapon drawn off a mount leaves it in the hand at once: its
            // mount clone stands in until ROCK shows the real weapon.
            const unsigned handIndex=destination.hand==api::Hand::Left?1:0;
            auto* wand=handIndex?f4cf::f4vr::getLeftHandNode():f4cf::f4vr::getRightHandNode();
            const bool melee=*kind==storage::Kind::Melee || *kind==storage::Kind::Knife;
            const bool standIn=wand && _scene.beginStandIn(handIndex,id,*wand,!melee);
            player_inventory::Row item;
            if(!storage.use(id,ui::ActionKind::ToggleWeapon,action.generation,&item)) {
                if(standIn) _scene.endStandIn(handIndex,true);
                completeItemAction("Weapon changed or unavailable; inventory refreshed",false); return;
            }
            _weaponDrawBusy=true;
            { std::scoped_lock lock(_storageMailbox);
              _standInFeeds[handIndex]={};
              _weaponDraw=WeaponDraw{item.formId,item.uniqueBase,destination.worldGeneration,destination.skeletonGeneration,
                  destination.providerGeneration,item.uniqueIndex,destination.hand,standIn}; }
            _realtimeUpdatesRequested=true;
            completeItemAction("Equipping weapon in the drawing hand",true);
            return;
        }
        const bool dropping=action.kind==ui::ActionKind::Drop;
        auto initial=destination.position;
        if(dropping) {
            api::core::Snapshot core;
            if(_rock.client.core()->getSnapshot(_rock.client.owner(),&core)!=api::Status::Ok) return;
            const auto sample=_rock.sample(core); const auto* player=RE::PlayerCharacter::GetSingleton();
            if(!sample.headValid || !player) { completeItemAction("Player pose is not ready for dropping",false); return; }
            const auto& head=sample.head.transform; const auto& forward=sample.head.forwardWorld; const auto p=player->GetPosition();
            const auto target=drop_policy::nearHand({initial.x,initial.y,initial.z},
                {head.translate[0],head.translate[1],head.translate[2]},{forward[0],forward[1],forward[2]},{p.x,p.y,p.z});
            if(!target) { completeItemAction("Drop pose is unavailable",false); return; }
            initial={target->x,target->y,target->z};
        }
        const RE::NiPoint3 position{initial.x, initial.y, initial.z};
        const auto delivery = storage.withdraw(id, position,action.generation);
        if (!delivery.handle) { completeItemAction("Item changed, unavailable, or protected by the game", false); return; }
        _retrievalBusy = true;
        _retrievalHand = destination.hand;
        if (!dropping) if (const auto ref = delivery.handle.get()) _deliveries[destination.hand == api::Hand::Left ? 1 : 0] = ref->formID;
        _retrievalTransaction = {.phase = RetrievalTransactionPhase::AwaitReference, .kind = dropping?RetrievalKind::PanelDrop:RetrievalKind::PanelGrab,
            .hand = destination.hand, .handle = delivery.handle, .started = std::chrono::steady_clock::now(), .itemName = action.rowId.data()};
        _retrievalTransaction.request.worldGeneration = destination.worldGeneration;
        _retrievalTransaction.request.skeletonGeneration = destination.skeletonGeneration;
        _retrievalTransaction.request.providerGeneration = destination.providerGeneration;
        _retrievalTransaction.request.dropPosition = initial;
        _realtimeUpdatesRequested = true;
        if(dropping) {
            // A hidden container can belong to an older cell. Move its new
            // reference into the player's cell before waiting for scene loading.
            const auto ref=delivery.handle.get();
            if(!ref || !native_inventory::showAt(*ref,position)) { failRetrievalTransaction("initial drop placement failed"); return; }
            logger::info("Backpack drop pending: ref={:08X}, initial=({:.2f},{:.2f},{:.2f}); awaiting loaded physics",ref->formID,position.x,position.y,position.z);
        }
        if(dropping) { _inventoryStatus="Placing item nearby"; sendInventoryModel(); }
        else completeItemAction("Sending the exact stored item to your hand",true);
    }
}
