#include "PCH.h"

#include "BackpackRuntime.h"
#include "BackpackReturnPolicy.h"
#include "BackpackDropPolicy.h"
#include "BackpackWeight.h"
#include "ui/PreviewInventory.h"
#include "BackpackItemService.h"

#include "vrcf/VRControllersManager.h"
#include <Windows.h>

namespace rock_backpack
{
    namespace
    {
        constexpr std::string_view kPluginName = "ROCK_Backpack.esp";
        constexpr std::uint32_t kAnchorFormListLocalId = 0x800;
        // ROCK_Backpack.esp owns the bundled MilitaryBackpack02 ARMO and its
        // complete asset contract, so the runtime has no collection-plugin
        // dependency even when that collection is absent.
        constexpr std::uint32_t kBackpackLocalFormId = 0x803;

        // Local ROCK/SCISSORS/CommonLibF4VR authority uses 70 game units per metre.
        constexpr float kGameUnitsPerMeter = 70.0f;
        constexpr float kVisibleDistanceGameUnits = 10.0f * kGameUnitsPerMeter;
        constexpr float kVisibleDistanceSquared = kVisibleDistanceGameUnits * kVisibleDistanceGameUnits;

        constexpr std::uint32_t kRetrievalGripButtonId =
            static_cast<std::uint32_t>(f4cf::vrcf::k_EButton_Grip);
        constexpr float kRetrievalForceGrabDistanceGameUnits = 32.0f;

        constexpr auto kIdleUpdateInterval = std::chrono::milliseconds(250);
        constexpr auto kVisibleUpdateInterval = std::chrono::milliseconds(11);
        constexpr auto kAnchorScanInterval = std::chrono::milliseconds(250);
        constexpr auto kConfigPollInterval = std::chrono::milliseconds(250);
        constexpr auto kInventoryRefreshInterval = std::chrono::milliseconds(100);
        // Vitals move smoothly rather than in discrete events, so they are
        // sampled less often than the row model and only published on a change
        // the reader could actually see.
        constexpr auto kVitalsRefreshInterval = std::chrono::milliseconds(250);
        constexpr float kVitalsEpsilon = 0.05f;
        constexpr auto kUiHapticMinimumInterval =
            std::chrono::milliseconds(35);
        constexpr auto kRetrievalTransactionTimeout =
            std::chrono::seconds(3);

        [[nodiscard]] bool vitalsChanged(
            const player_inventory::Vitals& previous,
            const player_inventory::Vitals& current) noexcept
        {
            const auto moved = [](const float left, const float right) noexcept {
                return std::abs(left - right) >= kVitalsEpsilon;
            };
            return previous.available != current.available ||
                   previous.capacityKnown != current.capacityKnown ||
                   moved(previous.carriedWeight, current.carriedWeight) ||
                   moved(previous.carryCapacity, current.carryCapacity) ||
                   moved(previous.health, current.health) ||
                   moved(previous.maxHealth, current.maxHealth);
        }

        [[nodiscard]] bool finitePoint(const RE::NiPoint3& point) noexcept
        {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }

        [[nodiscard]] float distanceSquared(const RE::NiPoint3& lhs, const RE::NiPoint3& rhs) noexcept
        {
            const float x = lhs.x - rhs.x;
            const float y = lhs.y - rhs.y;
            const float z = lhs.z - rhs.z;
            return x * x + y * y + z * z;
        }

        [[nodiscard]] bool sharesActiveWorldContext(const RE::TESObjectCELL* playerCell, const RE::TESObjectCELL* referenceCell) noexcept
        {
            if (!playerCell || !referenceCell) {
                return false;
            }
            if (playerCell == referenceCell) {
                return true;
            }
            return playerCell->IsExterior() && referenceCell->IsExterior() &&
                   playerCell->worldSpace && playerCell->worldSpace == referenceCell->worldSpace;
        }

        [[nodiscard]] panel_math::StoredTransform copyTransform(const RE::NiTransform& transform) noexcept
        {
            panel_math::StoredTransform copy{};
            copy.rotation = {
                transform.rotate.entry[0][0], transform.rotate.entry[0][1], transform.rotate.entry[0][2],
                transform.rotate.entry[1][0], transform.rotate.entry[1][1], transform.rotate.entry[1][2],
                transform.rotate.entry[2][0], transform.rotate.entry[2][1], transform.rotate.entry[2][2],
            };
            copy.translation = { transform.translate.x, transform.translate.y, transform.translate.z };
            copy.scale = transform.scale;
            return copy;
        }

    }

    BackpackRuntime& BackpackRuntime::get() noexcept
    {
        static BackpackRuntime instance;
        return instance;
    }

    void BackpackRuntime::startScheduler() noexcept
    {
        bool expected = false;
        if (!_schedulerStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            _schedulerThread = std::jthread([this](const std::stop_token stopToken) {
                _config.initialize();
                auto nextConfigPoll = std::chrono::steady_clock::now() + kConfigPollInterval;
                while (!stopToken.stop_requested()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= nextConfigPoll) {
                        _config.poll();
                        nextConfigPoll = now + kConfigPollInterval;
                    }
                    scheduleFrameTask();
                    const auto interval = _realtimeUpdatesRequested.load(std::memory_order_acquire) ?
                        kVisibleUpdateInterval : kIdleUpdateInterval;
                    std::this_thread::sleep_for(interval);
                }
            });
            logger::info("ROCK Backpack started serialized F4SE tasks; Grab requests, polling and cancellation run on ROCK's owner callback.");
        } catch (const std::exception& error) {
            _schedulerStarted.store(false, std::memory_order_release);
            logger::error("ROCK Backpack: failed to start its F4SE task scheduler ({}).", error.what());
        } catch (...) {
            _schedulerStarted.store(false, std::memory_order_release);
            logger::error("ROCK Backpack: failed to start its F4SE task scheduler.");
        }
    }

    void BackpackRuntime::scheduleFrameTask() noexcept
    {
        bool expected = false;
        if (!_frameTaskScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        const auto* tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            _frameTaskScheduled.store(false, std::memory_order_release);
            return;
        }
        try {
            tasks->AddTask([this]() {
                onFrame();
                _frameTaskScheduled.store(false, std::memory_order_release);
            });
        } catch (...) {
            _frameTaskScheduled.store(false, std::memory_order_release);
        }
    }

    bool BackpackRuntime::registerRockConsumer(const bool logFailure) noexcept
    {
        if (_rockFrameCallbackToken.load(std::memory_order_acquire)) return true;
        if (!_rock.connect()) {
            if (logFailure) logger::warn("ROCK Backpack is waiting for the current ROCK modular providers.");
            return false;
        }
        _rockOwnerToken.store(_rock.client.owner(), std::memory_order_release);
        std::uint64_t callback{};
        const auto result = _rock.client.core()->registerFrameCallback(
            _rock.client.owner(), &BackpackRuntime::onRockFrame, this, &callback);
        if (result != rock::api::Status::Ok) {
            if (logFailure) logger::warn("ROCK Backpack frame subscription rejected: {}", static_cast<unsigned>(result));
            return false;
        }
        _rockFrameCallbackToken.store(callback, std::memory_order_release);
        logger::info("ROCK Backpack registered owner frame callback {} on thread {}.", callback, GetCurrentThreadId());
        return true;
    }

    void ROCK_CALL BackpackRuntime::onRockFrame(
        const rock::api::core::Snapshot* snapshot, void* userData) noexcept
    {
        if (!snapshot || !userData) return;
        auto& runtime = *static_cast<BackpackRuntime*>(userData);
        runtime.processRockFrame(runtime._rock.sample(*snapshot));
    }

    bool BackpackRuntime::resolveAnchorRegistry() noexcept
    {
        _allowedBaseFormCount = 0;
        _backpackBaseFormId = 0;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        auto* formList = dataHandler ? dataHandler->LookupForm<RE::BGSListForm>(kAnchorFormListLocalId, kPluginName) : nullptr;
        if (!formList) {
            logger::critical(
                "ROCK Backpack could not resolve bundled anchor registry {:X} from '{}'.",
                kAnchorFormListLocalId,
                kPluginName);
            return false;
        }

        bool overflow = false;
        formList->ForEachForm([this, &overflow](RE::TESForm* form) {
            if (!form) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (_allowedBaseFormCount >= _allowedBaseFormIds.size()) {
                overflow = true;
                return RE::BSContainer::ForEachResult::kStop;
            }
            _allowedBaseFormIds[_allowedBaseFormCount++] = form->GetFormID();
            return RE::BSContainer::ForEachResult::kContinue;
        });

        if (overflow) {
            logger::critical("ROCK Backpack anchor registry exceeds the explicit {}-form safety limit.",
                _allowedBaseFormIds.size());
            _allowedBaseFormCount = 0;
            return false;
        }
        if (_allowedBaseFormCount == 0) {
            logger::critical("ROCK Backpack anchor registry is empty; the runtime will remain disabled.");
            return false;
        }

        auto end = _allowedBaseFormIds.begin() + static_cast<std::ptrdiff_t>(_allowedBaseFormCount);
        std::sort(_allowedBaseFormIds.begin(), end);
        end = std::unique(_allowedBaseFormIds.begin(), end);
        _allowedBaseFormCount = static_cast<std::size_t>(end - _allowedBaseFormIds.begin());

        auto* canonicalForm = dataHandler ?
                                  dataHandler->LookupForm(
                                      kBackpackLocalFormId,
                                      kPluginName) :
                                  nullptr;
        auto* canonicalObject = canonicalForm ?
                                    canonicalForm->As<RE::TESBoundObject>() :
                                    nullptr;
        if (!canonicalObject ||
            !isAllowedBaseForm(canonicalObject->GetFormID())) {
            logger::critical(
                "ROCK Backpack could not bind bundled inventory item {:06X} from '{}'; the plugin-owned registry must contain that exact object.",
                kBackpackLocalFormId,
                kPluginName);
            _allowedBaseFormCount = 0;
            return false;
        }
        _backpackBaseFormId = canonicalObject->GetFormID();
        auto* storage = dataHandler->LookupForm(0x805, kPluginName);
        if (!storage || storage->GetFormType() != RE::ENUM_FORM_ID::kCONT) {
            logger::critical("Backpack storage/container records are missing from the deployed plugin"); return false;
        }
        _storageBaseForm = storage->formID;
        BackpackStorage::get().setContainerBase(_storageBaseForm);
        logger::info("ROCK Backpack loaded {} eligible anchor base form(s) from the plugin-owned registry.",
            _allowedBaseFormCount);
        logger::info(
            "ROCK Backpack bound bundled inventory item {:08X} ({:06X} from '{}').",
            _backpackBaseFormId,
            kBackpackLocalFormId,
            kPluginName);
        return true;
    }

    void BackpackRuntime::onGameDataReady() noexcept
    {
        std::scoped_lock control(_controlMutex);
        if (_gameDataReady.load(std::memory_order_acquire)) {
            return;
        }
        if (!resolveAnchorRegistry()) {
            return;
        }
        if (!BackpackStorage::get().attachEvents() || !backpack_weight::available()) {
            logger::error("Backpack remains inactive: native storage startup did not complete"); return;
        }

        _gameDataReady.store(true, std::memory_order_release);
        // Core permits callback bootstrap before FrameBegin establishes its
        // owner thread. Do not defer this registration until a worker load task.
        (void)registerRockConsumer(true);
        startScheduler();

    }

    void BackpackRuntime::onPreLoadGame() noexcept
    {
        std::scoped_lock control(_controlMutex);
        _scene.clear(true); BackpackStorage::get().beforeLoad(); resetStorageInteraction();
        _sessionActive.store(false, std::memory_order_release);
        _grabDispatch.cancel(_retrievalTransaction.dispatchTicket, true);
        _backpackFeatureEnabled.store(false, std::memory_order_release);
        BackpackStorage::get().setPolicyActive(false);
        _backpackOnBack.store(false, std::memory_order_release);
        _retrievalBusy.store(false, std::memory_order_release);
        _previousFeatureEnabled = false;
        _retrievalTransaction = {};

        {
            std::scoped_lock lock(_retrievalMutex);
            _retrievalState = {};
            _retrievalRequest = {};
        }
        {
            std::scoped_lock lock(_hapticMutex);
            _pendingLeftHaptic = HapticCue::None;
            _pendingRightHaptic = HapticCue::None;
            _nextUiHapticAllowed = {};
            _hapticActiveUntil = {};
        }
        _realtimeUpdatesRequested.store(false, std::memory_order_release);
        _anchorScanRequested.store(true, std::memory_order_release);
        clearTrackedState();
        hidePanelAndResetSpatialState();
        (void)_panel.retire();
        (void)_opener.retire();
        (void)_pullMarker.retire();
            for (auto& guide:_guides) (void)guide.retire();
        resetInventoryModel("Waiting for game session");
        scheduleFrameTask();
    }

    void BackpackRuntime::onGameSessionReady(const bool resetTrackedState) noexcept
    {
        std::scoped_lock control(_controlMutex);
        _grabDispatch.cancel(_retrievalTransaction.dispatchTicket, true);
        if (resetTrackedState) {
            clearTrackedState();
        }
        _sessionActive.store(true, std::memory_order_release);
        _backpackFeatureEnabled.store(false, std::memory_order_release);
        BackpackStorage::get().setPolicyActive(false);
        _backpackOnBack.store(false, std::memory_order_release);
        _retrievalBusy.store(false, std::memory_order_release);
        _previousFeatureEnabled = false;
        _retrievalTransaction = {};

        {
            std::scoped_lock lock(_retrievalMutex);
            _retrievalState = {};
            _retrievalRequest = {};
        }
        {
            std::scoped_lock lock(_hapticMutex);
            _pendingLeftHaptic = HapticCue::None;
            _pendingRightHaptic = HapticCue::None;
            _nextUiHapticAllowed = {};
            _hapticActiveUntil = {};
        }
        _anchorScanRequested.store(true, std::memory_order_release);
        resetInventoryModel("Inventory link ready");
        sendInventoryModel();
        scheduleFrameTask();
        logger::info(
            "ROCK Backpack: {} session ready; anchor discovery remains lazy until the scheduled game task.",
            resetTrackedState ? "new-game" : "loaded-game");
    }

    bool BackpackRuntime::isAllowedBaseForm(const std::uint32_t formId) const noexcept
    {
        const auto end = _allowedBaseFormIds.begin() + static_cast<std::ptrdiff_t>(_allowedBaseFormCount);
        return std::binary_search(_allowedBaseFormIds.begin(), end, formId);
    }

    void BackpackRuntime::removeTrackedHandle(const std::uint32_t nativeHandle) noexcept
    {
        if (nativeHandle == 0) {
            return;
        }

        bool removed = false;
        {
            std::scoped_lock lock(_trackerMutex);
            for (std::size_t i = 0; i < _trackedAnchorCount; ++i) {
                if (_trackedAnchors[i].nativeHandle != nativeHandle) {
                    continue;
                }
                --_trackedAnchorCount;
                if (i != _trackedAnchorCount) {
                    _trackedAnchors[i] = _trackedAnchors[_trackedAnchorCount];
                }
                _trackedAnchors[_trackedAnchorCount] = {};
                if (_activeNativeHandle.load(std::memory_order_acquire) == nativeHandle) {
                    _activeNativeHandle.store(0, std::memory_order_release);
                }
                _trackerOverflowLogged.store(false, std::memory_order_release);
                removed = true;
                break;
            }
        }
        if (removed) {
            logger::info("ROCK Backpack removed loose anchor handle 0x{:X}.", nativeHandle);
        }
    }

    void BackpackRuntime::clearTrackedState() noexcept
    {
        {
            std::scoped_lock lock(_trackerMutex);
            for (std::size_t i = 0; i < _trackedAnchorCount; ++i) {
                _trackedAnchors[i] = {};
            }
            _trackedAnchorCount = 0;
            _activeNativeHandle.store(0, std::memory_order_release);
            _trackerOverflowLogged.store(false, std::memory_order_release);
        }
    }

    void BackpackRuntime::rebuildCurrentCellAnchors() noexcept
    {
        if (!_gameDataReady.load(std::memory_order_acquire) ||
            !_sessionActive.load(std::memory_order_acquire)) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell) {
            clearTrackedState();
            return;
        }

        std::array<TrackedAnchor, kMaximumTrackedAnchors> discovered{};
        std::size_t discoveredCount = 0;
        bool overflow = false;
        for (const auto& reference : cell->references) {
            if (!reference || reference->IsDeleted() || reference->IsDisabled()) {
                continue;
            }
            auto* baseForm = reference->GetObjectReference();
            if (!baseForm || !isAllowedBaseForm(baseForm->GetFormID())) {
                continue;
            }

            RE::ObjectRefHandle handle{ reference.get() };
            const std::uint32_t nativeHandle = handle.native_handle();
            if (nativeHandle == 0) {
                continue;
            }
            bool duplicate = false;
            for (std::size_t i = 0; i < discoveredCount; ++i) {
                if (discovered[i].nativeHandle == nativeHandle) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (discoveredCount >= discovered.size()) {
                overflow = true;
                break;
            }
            discovered[discoveredCount++] = TrackedAnchor{ handle, nativeHandle };
        }

        const std::uint32_t previousActive = _activeNativeHandle.load(std::memory_order_acquire);
        bool activeStillPresent = false;
        for (std::size_t i = 0; i < discoveredCount; ++i) {
            if (discovered[i].nativeHandle == previousActive) {
                activeStillPresent = true;
                break;
            }
        }

        std::size_t previousCount = 0;
        {
            std::scoped_lock lock(_trackerMutex);
            previousCount = _trackedAnchorCount;
            _trackedAnchors = discovered;
            _trackedAnchorCount = discoveredCount;
            _activeNativeHandle.store(activeStillPresent ? previousActive : 0, std::memory_order_release);
        }
        const bool wasOverflowing = _trackerOverflowLogged.exchange(overflow, std::memory_order_acq_rel);
        if (overflow && !wasOverflowing) {
            logger::warn("ROCK Backpack current-cell scan reached the {}-anchor safety limit.", discovered.size());
        }
        if (previousCount != discoveredCount) {
            logger::info("ROCK Backpack current-cell scan found {} eligible loose anchor(s).", discoveredCount);
        }
    }

    void BackpackRuntime::resetRetrievalGesture() noexcept
    {
        std::scoped_lock lock(_retrievalMutex);
        _retrievalState = {};
    }

    void BackpackRuntime::processBackpackRetrievalFrame(const RockFrame& snapshot) noexcept
    {
        using namespace rock::api;
        const bool eligible = _sessionActive.load() && _backpackFeatureEnabled.load() && _backpackOnBack.load() &&
            !_retrievalBusy.load() && snapshot.providerReady && snapshot.frikSkeletonReady && !snapshot.menuBlocking && !snapshot.configBlocking &&
            snapshot.handsValid && snapshot.headValid && !_weaponDrawBusy.load() &&
            !_visiblePulls[1].gesture.latched && !_zoneGestures[1].releasing && !_zoneGestures[1].queued &&
            !_zoneGestures[1].reference && !_zoneGestures[1].equipped;
        _bagPullReserved=_retrievalBusy.load() && _retrievalHand.load()==Hand::Left;
        const retrieval_policy::Vec3 hmd{snapshot.head.transform.translate[0], snapshot.head.transform.translate[1], snapshot.head.transform.translate[2]};
        const retrieval_policy::Vec3 forward{snapshot.head.forwardWorld[0], snapshot.head.forwardWorld[1], snapshot.head.forwardWorld[2]};
        PullHint pull;
        pull.frame=snapshot.sample.frameIndex; pull.bag=_activeBagForm.load();
        pull.world=snapshot.sample.worldGeneration; pull.skeleton=snapshot.sample.skeletonGeneration; pull.provider=snapshot.sample.providerGeneration;
        pull.eye={hmd.x,hmd.y,hmd.z}; pull.forward={forward.x,forward.y,forward.z};
        pull.right={snapshot.head.transform.rotate[0],snapshot.head.transform.rotate[1],snapshot.head.transform.rotate[2]};
        for (const unsigned h : {1u, 0u}) {
            const auto hand = h ? Hand::Left : Hand::Right;
            input::RawWandButtonState grip;
            const bool raw = _rock.input && _rock.input->getRawWandButtonState(_rock.client.owner(), hand, kRetrievalGripButtonId, &grip) == Status::Ok &&
                grip.available && grip.sampleAgeMilliseconds<150;
            const auto& pose = h ? snapshot.leftHandTransform : snapshot.rightHandTransform;
            const retrieval_policy::Vec3 position{pose.translate[0], pose.translate[1], pose.translate[2]};
            const auto zone = retrieval_policy::sampleHeadBackZone(position,hmd,forward,
                {snapshot.head.transform.rotate[0],snapshot.head.transform.rotate[1],snapshot.head.transform.rotate[2]});
            bool committed = false;
            const bool handEligible=eligible && h==1 && !snapshot.leftHolding;
            {
                std::scoped_lock lock(_retrievalMutex);
                const auto previous=_retrievalState[h].phase;
                const retrieval_policy::Input input{
                    .eligible = handEligible,
                    .rawGripAvailable = raw, .gripHeld = raw && grip.held, .zone = zone};
                if(h==1) _bagPullReserved|=retrieval_policy::reservesHand(_retrievalState[h],input);
                const auto outcome = retrieval_policy::advance(_retrievalState[h],input);
                if(h==1 && previous!=_retrievalState[h].phase && snapshot.sample.frameIndex>=_nextPullDiagnostic) {
                    _nextPullDiagnostic=snapshot.sample.frameIndex+15;
                    logger::info("Backpack pull transition: frame={} phase={}->{} eligible={} raw={} age={} down={} zone(valid/enter/exit)={}/{}/{} reserved={} holding={} flags={:X} weaponDraw={} authoredLatch={} stash(release/queued)={}/{} outcome={}",
                        snapshot.sample.frameIndex,unsigned(previous),unsigned(_retrievalState[h].phase),handEligible,raw,grip.sampleAgeMilliseconds,grip.held,
                        zone.valid,zone.withinEnter,zone.withinExit,_bagPullReserved,snapshot.leftHolding,snapshot.handStates[h].flags,_weaponDrawBusy.load(),
                        _visiblePulls[1].gesture.latched,_zoneGestures[1].releasing,_zoneGestures[1].queued,unsigned(outcome));
                }
                if(h==1) {
                    using Prompt=retrieval_policy::Prompt;
                    const auto prompt=retrieval_policy::prompt(_retrievalState[h],handEligible,raw,zone);
                    pull.visible=prompt!=Prompt::Hidden;
                    pull.prompt.action=prompt==Prompt::Pull?ui::PullPrompt::Action::PullBag:
                        prompt==Prompt::Grip?ui::PullPrompt::Action::GripBag:ui::PullPrompt::Action::RearmBag;
                    if(previous==retrieval_policy::Phase::ReadyOutside && _retrievalState[h].phase==retrieval_policy::Phase::InsideNeutral)
                        _entryCueHands.fetch_or(2u);
                }
                if (outcome == retrieval_policy::Outcome::PullCommitted && !_retrievalRequest.pending) {
                    bool expected = false;
                    if (_retrievalBusy.compare_exchange_strong(expected, true)) {
                        _retrievalRequest = {.pending = true, .hand = hand, .dropPosition = position,
                            .worldGeneration = snapshot.sample.worldGeneration, .skeletonGeneration = snapshot.sample.skeletonGeneration, .providerGeneration = snapshot.sample.providerGeneration};
                        _backpackOnBack = false; committed = true;
                        _retrievalHand = hand;
                    }
                }
            }
            if (committed) {
                _realtimeUpdatesRequested = true; scheduleFrameTask();
                logger::info("Backpack {}-hand left-shoulder pull queued", h ? "left" : "right");
            }
        }
        { std::unique_lock lock(_storageMailbox,std::try_to_lock); if(lock) _pullHint=pull; }
    }

    void BackpackRuntime::serviceBackpackOwnership(const bool hasLooseWorldInstance) noexcept
    {
        if (!_backpackFeatureEnabled.load() || !_backpackBaseFormId) return;
        try {
            auto& storage = BackpackStorage::get();
            RE::ObjectRefHandle existing;
            if (hasLooseWorldInstance) {
                std::scoped_lock lock(_trackerMutex);
                if (_trackedAnchorCount) existing = _trackedAnchors[0].handle;
            }
            const auto handle = storage.ensureBackpack(_backpackBaseFormId, existing);
            if (!handle) return;
            _backpackOnBack = storage.onBack();
            if (_backpackOnBack.load() || !_activeBagForm.load()) {
                const auto ref=handle.get();
                if(ref && storage.select(*ref,_storageBaseForm)) {
                    const bool changed=_activeBagForm.exchange(ref->formID)!=ref->formID;
                    if(changed) _inventoryModelDirty=true;
                    refreshInventoryModel(changed);
                }
            }
        } catch (...) { logger::error("Managed backpack adoption failed; existing contents preserved"); }
    }
    void BackpackRuntime::failRetrievalTransaction(
        const char* reason) noexcept
    {
        bool canReturn = true;
        if (_retrievalTransaction.dispatchTicket) {
            _grabDispatch.cancel(_retrievalTransaction.dispatchTicket);
            const auto reply = _grabDispatch.read(_retrievalTransaction.dispatchTicket);
            if (reply && reply->phase != GrabCommandDispatch::Phase::Finished) {
                if (_retrievalTransaction.started != std::chrono::steady_clock::time_point{})
                    logger::warn("ROCK Backpack: waiting for owner-thread cancellation of ticket {} ({}).",
                        _retrievalTransaction.dispatchTicket, reason ? reason : "unspecified");
                _retrievalTransaction.started = {};
                return;
            }
            if (reply && reply->succeeded) {
                finishRetrievalTransaction("completed before cancellation");
                return;
            }
            canReturn = reply && reply->mayReturnToInventory;
        }
        const bool hadMaterializedReference =
            static_cast<bool>(_retrievalTransaction.handle);
        if (const auto ref = _retrievalTransaction.handle.get(); ref && _rock.client.owner()) {
            rock::api::core::Snapshot core;
            if (_rock.client.core()->getSnapshot(_rock.client.owner(), &core) == rock::api::Status::Ok) {
                const auto frame = _rock.sample(core);
                if (!frame.handsValid) return; // Unknown ownership never authorizes parking a held object.
                if (frame.leftHeldFormId == ref->formID || frame.rightHeldFormId == ref->formID) {
                    _retrievalTransaction.hand = frame.leftHeldFormId == ref->formID ? rock::api::Hand::Left : rock::api::Hand::Right;
                    finishRetrievalTransaction("exact reference already acquired by a hand"); return;
                }
            } else return;
        }
        _grabDispatch.acknowledge(_retrievalTransaction.dispatchTicket);
        const bool panelItem = _retrievalTransaction.kind != RetrievalKind::BackpackPull;
        const bool returned = canReturn && hadMaterializedReference && (panelItem ?
            BackpackStorage::get().completeDelivery(false) : BackpackStorage::get().returnBackpack());
        if (hadMaterializedReference && !returned) {
            logger::error(
                "ROCK Backpack: retrieval failed ({}), and the exact loose reference could not be restored to its storage owner; the world reference is retained.",
                reason ? reason : "unspecified");
        } else {
            logger::warn(
                "ROCK Backpack: retrieval failed ({}); {}.",
                reason ? reason : "unspecified",
                returned ?
                    "the exact item was restored to its storage owner" :
                    "no world reference required rollback");
        }

        // Backpack ownership state and the physical pull gesture belong to the
        // back-zone pull only. A failed panel grab must not clear the gesture
        // the user may be performing, nor rewrite backpack ownership.
        const bool backpackPull =
            _retrievalTransaction.kind == RetrievalKind::BackpackPull;
        _retrievalTransaction = {};
        if (backpackPull) {
            {
                std::scoped_lock lock(_retrievalMutex);
                _retrievalRequest = {};
                _retrievalState = {};
            }
            _backpackOnBack.store(
                BackpackStorage::get().onBack(),
                std::memory_order_release);
        }
        _retrievalBusy.store(false, std::memory_order_release);
        _anchorScanRequested.store(true, std::memory_order_release);
        if(panelItem) completeItemAction(returned?"Placement failed; item returned to backpack":"Placement incomplete; item ownership retained",false);
    }

    void BackpackRuntime::finishRetrievalTransaction(
        const char* outcome) noexcept
    {
        _grabDispatch.acknowledge(_retrievalTransaction.dispatchTicket);
        const bool backpackPull =
            _retrievalTransaction.kind == RetrievalKind::BackpackPull;
        if (!backpackPull) (void)BackpackStorage::get().completeDelivery(true);
        const auto hand =
            _retrievalTransaction.hand ==
                    rock::api::Hand::Right ?
                ui::Hand::Right :
                ui::Hand::Left;
        if (backpackPull) {
            logger::info(
                "ROCK Backpack: retrieval transaction {}.",
                outcome ? outcome : "completed");
        } else {
            logger::info(
                "ROCK Backpack: panel {} of '{}' {}.",
                _retrievalTransaction.kind==RetrievalKind::PanelDrop?"drop":"grab",
                _retrievalTransaction.itemName,
                outcome ? outcome : "completed");
        }
        const bool dropped=_retrievalTransaction.kind==RetrievalKind::PanelDrop;
        _retrievalTransaction = {};
        _retrievalBusy.store(false, std::memory_order_release);
        if (backpackPull) {
            _backpackOnBack.store(
                false,
                std::memory_order_release);
        }
        _anchorScanRequested.store(true, std::memory_order_release);
        queueHaptic(hand, HapticCue::RetrievalCommitted);
        if(dropped) completeItemAction("Item placed nearby",true);
        if (backpackPull) {
            resetRetrievalGesture();
            native_inventory::backpackSound(_backpackBaseFormId, true);
        }
    }

    void BackpackRuntime::serviceBackpackRetrievalTransaction() noexcept
    {
        using namespace rock::api;

        if (_retrievalTransaction.phase ==
            RetrievalTransactionPhase::Idle) {
            RetrievalRequest request{};
            {
                std::scoped_lock lock(_retrievalMutex);
                if (_retrievalRequest.pending) {
                    request = _retrievalRequest;
                    _retrievalRequest = {};
                }
            }
            if (!request.pending) {
                return;
            }
            if (!_sessionActive.load(std::memory_order_acquire) ||
                !_backpackFeatureEnabled.load(std::memory_order_acquire)) {
                _retrievalBusy.store(false, std::memory_order_release);
                _backpackOnBack.store(
                    BackpackStorage::get().onBack(),
                    std::memory_order_release);
                resetRetrievalGesture();
                return;
            }

            const RE::NiPoint3 dropLocation{
                request.dropPosition.x,
                request.dropPosition.y,
                request.dropPosition.z,
            };
            const backpack_item::MaterializeResult materialized{
                .success = BackpackStorage::get().pullBackpack(dropLocation),
                .handle = BackpackStorage::get().backpack(), .reason = "persistent-reference"};
            if (!materialized.success) {
                logger::warn(
                    "ROCK Backpack: pull could not materialize the managed backpack ({}).",
                    materialized.reason);
                _retrievalBusy.store(false, std::memory_order_release);
                _backpackOnBack.store(
                    BackpackStorage::get().onBack(),
                    std::memory_order_release);
                resetRetrievalGesture();
                return;
            }

            _retrievalTransaction = {
                .phase = RetrievalTransactionPhase::AwaitReference,
                .kind = RetrievalKind::BackpackPull,
                .hand = request.hand,
                .handle = materialized.handle,
                .dispatchTicket = 0,
                .started = std::chrono::steady_clock::now(),
                .request = request,
            };
            _anchorScanRequested.store(true, std::memory_order_release);
            logger::info(
                "ROCK Backpack: enabled the managed backpack at the {} hand ({}).",
                request.hand == Hand::Left ? "left" : "right", materialized.reason);
        }

        const auto now = std::chrono::steady_clock::now();
        if (_retrievalTransaction.started !=
                std::chrono::steady_clock::time_point{} &&
            now - _retrievalTransaction.started >
                kRetrievalTransactionTimeout) {
            failRetrievalTransaction("transaction timeout");
            return;
        }

        if (_retrievalTransaction.phase ==
            RetrievalTransactionPhase::AwaitReference) {
            const auto reference = _retrievalTransaction.handle.get();
            if (!reference) {
                return;
            }
            if (reference->IsDeleted() || reference->IsDisabled()) {
                failRetrievalTransaction(
                    "materialized reference became unavailable");
                return;
            }

            // Disabled stored references rebuild their scene asynchronously.
            // Moving only before Enable leaves the rebuilt physics at its old
            // location. Place the loaded reference once, then let the native
            // scene/physics update finish before asking ROCK to attach it.
            if (!reference->Get3D()) return;
            const bool dropping=_retrievalTransaction.kind==RetrievalKind::PanelDrop;
            core::Snapshot dropFrame;
            RockFrame dropSample;
            if(dropping) {
                if(_rock.client.core()->getSnapshot(_rock.client.owner(),&dropFrame)!=Status::Ok) return;
                const auto& request=_retrievalTransaction.request;
                if(dropFrame.sample.worldGeneration!=request.worldGeneration || dropFrame.sample.skeletonGeneration!=request.skeletonGeneration ||
                    dropFrame.sample.providerGeneration!=request.providerGeneration) { failRetrievalTransaction("drop lifecycle changed"); return; }
                dropSample=_rock.sample(dropFrame);
                if(!dropSample.handsValid || !dropSample.headValid) return;
                if(dropSample.leftHeldFormId==reference->formID || dropSample.rightHeldFormId==reference->formID) {
                    finishRetrievalTransaction("acquired by a hand during placement"); return;
                }
            }
            if (!_retrievalTransaction.positionedAfterLoad) {
                core::Snapshot live;
                if (_rock.client.core()->getSnapshot(_rock.client.owner(), &live) != Status::Ok) return;
                if(dropping && (live.sample.worldGeneration!=dropFrame.sample.worldGeneration || live.sample.skeletonGeneration!=dropFrame.sample.skeletonGeneration ||
                    live.sample.providerGeneration!=dropFrame.sample.providerGeneration)) { failRetrievalTransaction("drop pose lifecycle changed"); return; }
                const auto sample = _rock.sample(live); if (!sample.handsValid) return;
                const auto& pose = _retrievalTransaction.hand == Hand::Left ? sample.leftHandTransform : sample.rightHandTransform;
                retrieval_policy::Vec3 point{pose.translate[0],pose.translate[1],pose.translate[2]};
                if(dropping) {
                    const auto* player=RE::PlayerCharacter::GetSingleton(); if(!player) return;
                    const auto p=player->GetPosition(); const auto& head=sample.head.transform; const auto& forward=sample.head.forwardWorld;
                    const auto target=sample.headValid?drop_policy::nearHand({point.x,point.y,point.z},
                        {head.translate[0],head.translate[1],head.translate[2]},{forward[0],forward[1],forward[2]},{p.x,p.y,p.z}):std::nullopt;
                    if(!target) { failRetrievalTransaction("drop target is not near a valid hand"); return; }
                    point={target->x,target->y,target->z};
                }
                if (!native_inventory::showAt(*reference, {point.x, point.y, point.z})) {
                    failRetrievalTransaction("loaded reference positioning failed");
                    return;
                }
                _retrievalTransaction.positionedAfterLoad = true;
                if(dropping) { _retrievalTransaction.positionedFrame=live.sample.frameIndex; _retrievalTransaction.request.dropPosition=point; }
                return;
            }

            if(dropping) {
                const auto actual=reference->GetPosition(); const auto expected=_retrievalTransaction.request.dropPosition;
                const auto* player=RE::PlayerCharacter::GetSingleton();
                const auto result=drop_policy::verify(dropFrame.sample.frameIndex,_retrievalTransaction.positionedFrame,
                    {actual.x,actual.y,actual.z},{expected.x,expected.y,expected.z},player && reference->GetParentCell()==player->GetParentCell());
                if(result==drop_policy::Placement::Wait) return;
                const auto visual=reference->Get3D()->world.translate;
                logger::info("Backpack drop placement: ref={:08X}, expected=({:.2f},{:.2f},{:.2f}), actual=({:.2f},{:.2f},{:.2f}), scene=({:.2f},{:.2f},{:.2f}), confirmed={}",
                    reference->formID,expected.x,expected.y,expected.z,actual.x,actual.y,actual.z,visual.x,visual.y,visual.z,result==drop_policy::Placement::Confirmed);
                if(result==drop_policy::Placement::Confirmed) finishRetrievalTransaction("confirmed after loaded placement");
                else failRetrievalTransaction("loaded drop position did not match target");
                return; // World drops never dispatch a force grab.
            }

            const std::uint64_t ownerToken =
                _rockOwnerToken.load(std::memory_order_acquire);
            if (ownerToken == 0 || !_rock.grab ||
                !_rock.grab->requestForceGrab) {
                failRetrievalTransaction(
                    "ROCK force-grab service unavailable");
                return;
            }

            grab::ForceGrabRequest request{};
            request.hand = _retrievalTransaction.hand;
            request.targetFormId = reference->GetFormID();
            request.worldGeneration =
                _retrievalTransaction.request.worldGeneration;
            request.skeletonGeneration =
                _retrievalTransaction.request.skeletonGeneration;
            request.providerGeneration =
                _retrievalTransaction.request.providerGeneration;
            request.maxDistanceGame =
                kRetrievalForceGrabDistanceGameUnits;

            const auto ticket = _grabDispatch.submit(request);
            if (!ticket) {
                failRetrievalTransaction("previous provider command has not retired");
                return;
            }
            _retrievalTransaction.phase = RetrievalTransactionPhase::AwaitRockResult;
            _retrievalTransaction.dispatchTicket = ticket;
            logger::info("ROCK Backpack: submitted {}-hand grab ticket {} for {:08X} from task thread {}; waiting for ROCK owner callback.",
                request.hand == Hand::Left ? "left" : "right", ticket, request.targetFormId, GetCurrentThreadId());
        }

        if (_retrievalTransaction.phase != RetrievalTransactionPhase::AwaitRockResult) return;
        const auto reply = _grabDispatch.read(_retrievalTransaction.dispatchTicket);
        if (!reply) {
            failRetrievalTransaction("provider command ticket disappeared");
            return;
        }
        if (reply->phase == GrabCommandDispatch::Phase::Requested) return;
        if (!_retrievalTransaction.admissionLogged) {
            logger::info("ROCK Backpack: grab ticket {} owner-thread={} admission={} command={} query={}.",
                reply->ticket, reply->executorThread, static_cast<unsigned>(reply->admission),
                reply->commandId, static_cast<unsigned>(reply->query));
            _retrievalTransaction.admissionLogged = true;
        }
        if (reply->phase != GrabCommandDispatch::Phase::Finished) return;
        if (reply->succeeded) {
            logger::info("ROCK Backpack: grab ticket {} completed on owner thread {} for reference {:08X}.",
                reply->ticket, reply->executorThread, reply->result.targetFormId);
            finishRetrievalTransaction("succeeded through ROCK; its saved grab offset is authoritative");
            return;
        }
        logger::warn("ROCK Backpack: grab ticket {} failed on owner thread {}: admission={} query={} state={} failure={}; return-safe={}.",
            reply->ticket, reply->executorThread, static_cast<unsigned>(reply->admission),
            static_cast<unsigned>(reply->query), static_cast<unsigned>(reply->result.state),
            static_cast<unsigned>(reply->result.failure), reply->mayReturnToInventory);
        failRetrievalTransaction("ROCK owner-thread grab did not succeed");
    }

    void BackpackRuntime::processRockFrame(const RockFrame& snapshot) noexcept
    {
        // Registration and command admission share ROCK's owner callback. The
        // resolved base form is immutable after game-data-ready publication.
        if (!_gameDataReady.load(std::memory_order_acquire) ||
            !_rock.ensureBackpackPoses(_backpackBaseFormId)) return;
        if (!_grabWakeRegistered && _rock.grab && _rock.grab->setEventCallback) {
            const auto result = _rock.grab->setEventCallback(_rock.client.owner(), &onRockGrabEvent, this);
            _grabWakeRegistered = result == rock::api::Status::Ok;
            if (!_grabWakeRegistered && result != _grabWakeStatus)
                logger::warn("ROCK Backpack release-event wake registration failed: {}.", static_cast<unsigned>(result));
            _grabWakeStatus = result;
        }
        if (_rock.grab && _grabDispatch.pump(*_rock.grab, _rock.client.owner(), GetCurrentThreadId(), _rock.storage ? _rock.storage->draw : nullptr))
            scheduleFrameTask();
        processBackpackRetrievalFrame(snapshot);
        processStorageFrame(snapshot);
        protectBackpackInput(snapshot);
    }

    void ROCK_CALL BackpackRuntime::onRockGrabEvent(const rock::api::grab::Event* event, void* data) noexcept
    {
        // Borrowed producer callback: no ROCK calls or engine operations here.
        if (!event || !data || event->kind != static_cast<unsigned>(rock::api::grab::EventKind::Released)) return;
        auto& runtime = *static_cast<BackpackRuntime*>(data);
        if (runtime._sessionActive.load() && runtime._backpackFeatureEnabled.load()) runtime.scheduleFrameTask();
    }

    void BackpackRuntime::serviceBackpackReturn() noexcept
    {
        using namespace rock::api;
        if (!_sessionActive.load() || _retrievalBusy.load() || !_rock.client.owner()) return;
        auto& storage = BackpackStorage::get();
        const auto reference = storage.backpack().get(); if (!reference) return;
        if (reference->IsDisabled()) { _backpackOnBack = true; return; }
        core::Snapshot core;
        if (_rock.client.core()->getSnapshot(_rock.client.owner(), &core) != Status::Ok) return;
        const auto frame = _rock.sample(core);
        if (backpack_return::evaluate(reference->formID, _retrievalBusy.load(), frame) != backpack_return::Decision::Return) return;
        // Owning the exact reference makes return independent of event-ring
        // delivery, cell scans, throw speed and native inventory pickup.
        if (!storage.returnBackpack()) return;
        native_inventory::backpackSound(_backpackBaseFormId, false);
        removeTrackedHandle(reference->GetHandle().native_handle());
        _backpackOnBack = true; _anchorScanRequested = true; _inventoryModelDirty = true;
        hidePanelAndResetSpatialState(); resetRetrievalGesture();
        logger::info("Backpack parked on back with unchanged reference and contents: {:08X}", reference->formID);
    }
    void BackpackRuntime::hidePanelAndResetSpatialState() noexcept
    {
        _panel.hide();
        _opener.hide();
        for (auto& guide:_guides) guide.hide();
        // The leg slot remains available while its managed backpack is parked.
        // Only the bag-mounted scene/UI is hidden; lifecycle loss releases storage.
        if (_sessionActive.load() && _backpackFeatureEnabled.load()) {
            _scene.clearBackpack();
        } else if (_activeBagForm.exchange(0)) { _scene.clear(true); BackpackStorage::get().leave(); }
        publishCollisionDisplays();
        _inventoryOpen = false;
        _panelVisible = false;
        _actionHand = ui::Hand::None;
    }

    void BackpackRuntime::queueHaptic(
        const ui::Hand hand,
        const HapticCue cue) noexcept
    {
        if (hand == ui::Hand::None ||
            cue == HapticCue::None) {
            return;
        }

        try {
            std::scoped_lock lock(_hapticMutex);
            if (cue == HapticCue::Rejected) {
                const auto now = std::chrono::steady_clock::now();
                if (_nextUiHapticAllowed !=
                        std::chrono::steady_clock::time_point{} &&
                    now < _nextUiHapticAllowed) {
                    return;
                }
                _nextUiHapticAllowed =
                    now + kUiHapticMinimumInterval;
            }

            auto& pending =
                hand == ui::Hand::Left ?
                    _pendingLeftHaptic :
                    _pendingRightHaptic;
            if (static_cast<std::uint8_t>(cue) >=
                static_cast<std::uint8_t>(pending)) {
                pending = cue;
            }
        } catch (...) {
            // Haptics are feedback only. A failed queue must never alter an
            // inventory or retrieval transaction.
        }
    }

    void BackpackRuntime::queuePointerHaptic(const HapticCue cue) noexcept
    {
        queueHaptic(_actionHand, cue);
    }

    void BackpackRuntime::serviceHaptics() noexcept
    {
        const auto entries=_entryCueHands.exchange(0);
        ZoneHint hint;
        { std::scoped_lock lock(_storageMailbox); hint = _zoneHint; }
        rock::api::core::Snapshot snapshot;
        const auto now = std::chrono::steady_clock::now();
        const bool ready=_sessionActive.load() && _rock.client.core() &&
            _rock.client.core()->getSnapshot(_rock.client.owner(), &snapshot) == rock::api::Status::Ok &&
            snapshot.providerReady && !snapshot.menuBlocking && !snapshot.configBlocking;
        const bool enabled=_backpackFeatureEnabled.load();
        if(!ready || !enabled) _hapticActiveUntil={};
        if(ready && enabled) {
            if(entries & 1) queueHaptic(ui::Hand::Right,HapticCue::ZoneEntered);
            if(entries & 2) queueHaptic(ui::Hand::Left,HapticCue::ZoneEntered);
        }
        if (hint.readyHands && now >= _nextStashHaptic && ready &&
            snapshot.sample.frameIndex >= hint.frame && snapshot.sample.frameIndex - hint.frame <= 3) {
            _nextStashHaptic = now + std::chrono::milliseconds(200);
            if (hint.readyHands & 1) queueHaptic(ui::Hand::Right, HapticCue::StashReady);
            if (hint.readyHands & 2) queueHaptic(ui::Hand::Left, HapticCue::StashReady);
        }
        HapticCue left = HapticCue::None;
        HapticCue right = HapticCue::None;
        {
            std::scoped_lock lock(_hapticMutex);
            left = std::exchange(
                _pendingLeftHaptic,
                HapticCue::None);
            right = std::exchange(
                _pendingRightHaptic,
                HapticCue::None);
        }
        if (left == HapticCue::None && right == HapticCue::None && now>=_hapticActiveUntil) {
            return;
        }

        const auto intensity = [](const HapticCue cue) noexcept {
            switch (cue) {
            case HapticCue::StashReady:
                return 0.22f;
            case HapticCue::ZoneEntered:
                return 0.5f;
            case HapticCue::Rejected:
                return 0.38f;
            case HapticCue::WeaponCommitted:
                return 0.48f;
            case HapticCue::ArmorCommitted:
                return 0.56f;
            case HapticCue::ItemCommitted:
                return 0.52f;
            case HapticCue::RetrievalCommitted:
                return 0.78f;
            case HapticCue::None:
            default:
                return 0.0f;
            }
        };

        try {
            // Explicit Left/Right values are physical roles; handed-mode
            // mapping is irrelevant. Zone entry uses Virtual Holsters' 50 ms cue,
            // pumped by this existing task; other cues remain immediate pulses.
            f4cf::vrcf::VRControllers.update(false);
            if(left==HapticCue::ZoneEntered || right==HapticCue::ZoneEntered)
                _hapticActiveUntil=now+std::chrono::milliseconds(50);
            if (left != HapticCue::None) {
                f4cf::vrcf::VRControllers.triggerHaptic(
                    f4cf::vrcf::Hand::Left,
                    left==HapticCue::ZoneEntered?0.05f:0.0f,
                    intensity(left));
            }
            if (right != HapticCue::None) {
                f4cf::vrcf::VRControllers.triggerHaptic(
                    f4cf::vrcf::Hand::Right,
                    right==HapticCue::ZoneEntered?0.05f:0.0f,
                    intensity(right));
            }
        } catch (const std::exception& error) {
            logger::warn(
                "ROCK Backpack haptic feedback failed ({}).",
                error.what());
        } catch (...) {
            logger::warn(
                "ROCK Backpack haptic feedback failed.");
        }
    }

    void BackpackRuntime::resetInventoryModel(const char* status) noexcept
    {
        _nextInventoryRefresh = {}; _vitals = {}; _vitalsInitialized = false; _nextVitalsRefresh = {};
        _inventoryGeneration = BackpackStorage::get().generation();
        _inventoryStatus = status ? status : "Backpack ready";
        _inventoryModelDirty = true;
    }

    void BackpackRuntime::sendInventoryModel() noexcept
    {
        try {
            auto& storage = BackpackStorage::get();
            auto model = storage.model(_inventoryStatus);
            _inventoryGeneration = model->generation;
            model->vitalsAvailable = _vitals.available; model->capacityKnown = _vitals.capacityKnown;
            if(_vitals.available) model->weight=_vitals.carriedWeight;
            model->capacity = _vitals.carryCapacity; model->health = _vitals.health; model->maxHealth = _vitals.maxHealth;
            {
                std::scoped_lock lock(_storageMailbox);
                _spatialModel = model; _storedReferences = storage.storedReferences();
            }
            item_preview::request(storage.previews());
            if (model->available && _exportedInventoryGeneration != model->generation) {
                if (ui::savePreviewInventory(*model, ui::stimpakImagePath().parent_path() / "Inventory.json")) _exportedInventoryGeneration = model->generation;
                else logger::warn("Backpack desktop inventory capture could not be written");
            }
            _panel.publish(model); _opener.publish(model);
            _inventoryModelDirty = false;
        } catch (const std::exception& error) { logger::error("Backpack storage model failed: {}", error.what()); }
        catch (...) { logger::error("Backpack storage model failed"); }
    }

    void BackpackRuntime::refreshVitals(const bool force) noexcept
    {
        if (!_panelVisible) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            _nextVitalsRefresh !=
                std::chrono::steady_clock::time_point{} &&
            now < _nextVitalsRefresh) {
            return;
        }
        _nextVitalsRefresh = now + kVitalsRefreshInterval;

        auto current = player_inventory::captureVitals();
        // Health and carried weight are floats that drift by fractions between
        // ticks. Publishing only on a visible change keeps a resting player
        // from rebuilding its presentation every refresh interval.
        if (_vitalsInitialized && !vitalsChanged(_vitals, current)) {
            return;
        }
        _vitals = current;
        _vitalsInitialized = true;
        sendInventoryModel();
    }

    void BackpackRuntime::refreshInventoryModel(const bool force) noexcept
    {
        if (!_activeBagForm.load()) return;
        const auto now = std::chrono::steady_clock::now();
        if (force || _inventoryModelDirty.load() || _inventoryGeneration != BackpackStorage::get().generation() || now >= _nextInventoryRefresh) {
            _nextInventoryRefresh = now + kInventoryRefreshInterval;
            rock::api::core::Snapshot frame;
            if(_rock.client.core() && _rock.client.core()->getSnapshot(_rock.client.owner(),&frame)==rock::api::Status::Ok &&
                _sessionActive.load() && frame.providerReady && !frame.menuBlocking && !frame.configBlocking)
                BackpackStorage::get().restoreStoredReferences();
            sendInventoryModel();
        }
    }

    void BackpackRuntime::completeItemAction(
        const char* status,
        const bool committed)
    {
        _inventoryStatus = status;
        _inventoryModelDirty.store(true, std::memory_order_release);
        _nextInventoryRefresh =
            std::chrono::steady_clock::now() + kInventoryRefreshInterval;
        sendInventoryModel();
        // Weight moved, so republish vitals without waiting for the next tick.
        refreshVitals(true);
        queuePointerHaptic(
            committed ? HapticCue::ItemCommitted : HapticCue::Rejected);
    }

    BackpackRuntime::FreeHandDelivery
    BackpackRuntime::resolveFreeHandDelivery() noexcept
    {
        using Hand = ui::Hand;

        rock::api::core::Snapshot core;
        if (!_rock.client.owner() || _rock.client.core()->getSnapshot(_rock.client.owner(), &core) != rock::api::Status::Ok) {
            return FreeHandDelivery{ .reason = "handTrackingUnavailable" };
        }
        const auto sample = _rock.sample(core);
        if (!sample.handsValid) return FreeHandDelivery{ .reason = "handTrackingUnavailable" };

        /*
         * Occupancy decides the receiving hand, not which hand is pointing.
         * This panel is anchored to the backpack, and the backpack is normally
         * held, so the hand that is not driving the pointer is usually the one
         * already full. Delivering there force-grabs into an occupied hand and
         * the item goes nowhere visible.
         *
         * Preference order: a hand ROCK reports as free, then the pointer hand
         * as the tie-break when both are free, since that is the hand the user
         * is already directing at the item.
         */
        const bool pointerIsLeft = _actionHand == Hand::Left;
        bool deliverLeft = false;
        if (!sample.leftHolding && !sample.rightHolding) {
            deliverLeft = pointerIsLeft;
        } else if (!sample.leftHolding) {
            deliverLeft = true;
        } else if (!sample.rightHolding) {
            deliverLeft = false;
        } else {
            return FreeHandDelivery{ .reason = "bothHandsHolding" };
        }

        return FreeHandDelivery{
            .valid = true,
            .reason = "ok",
            .hand = deliverLeft ?
                        rock::api::Hand::Left :
                        rock::api::Hand::Right,
            .position = deliverLeft ?
                retrieval_policy::Vec3{ sample.leftHandTransform.translate[0], sample.leftHandTransform.translate[1], sample.leftHandTransform.translate[2] } :
                retrieval_policy::Vec3{ sample.rightHandTransform.translate[0], sample.rightHandTransform.translate[1], sample.rightHandTransform.translate[2] },
            .worldGeneration = sample.sample.worldGeneration,
            .skeletonGeneration = sample.sample.skeletonGeneration,
            .providerGeneration = sample.sample.providerGeneration,
        };
    }

    void BackpackRuntime::onFrame() noexcept try
    {
        std::scoped_lock control(_controlMutex);
        if (!_gameDataReady.load(std::memory_order_acquire)) {
            return;
        }
        if (!_sessionActive.load(std::memory_order_acquire)) {
            _realtimeUpdatesRequested.store(false, std::memory_order_release);
            hidePanelAndResetSpatialState();
            (void)_panel.retire();
            (void)_opener.retire();
            (void)_pullMarker.retire();
            for (auto& guide:_guides) (void)guide.retire();
            return;
        }
        const float elapsedSeconds = sceneElapsedSeconds();

        if (!_rockFrameCallbackToken.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() >= _nextRockRetry) {
            _nextRockRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            (void)registerRockConsumer(true);
        }
        const BackpackSettings settings = _config.snapshot();
        _backpackFeatureEnabled.store(
            settings.panelEnabled,
            std::memory_order_release);
        BackpackStorage::get().setPolicyActive(settings.panelEnabled && _sessionActive.load());
        if (settings.panelEnabled != _previousFeatureEnabled) {
            resetRetrievalGesture();
            _previousFeatureEnabled = settings.panelEnabled;
        }

        serviceBackpackRetrievalTransaction();
        serviceStorageRequests();
        serviceStandIns(elapsedSeconds);
        serviceBackpackReturn();
        serviceHaptics();
        const auto now = std::chrono::steady_clock::now();
        if ((settings.panelEnabled ||
                _retrievalBusy.load(std::memory_order_acquire)) &&
            (_anchorScanRequested.exchange(
                 false,
                 std::memory_order_acq_rel) ||
                _nextAnchorScan ==
                    std::chrono::steady_clock::time_point{} ||
                now >= _nextAnchorScan)) {
            rebuildCurrentCellAnchors();
            _nextAnchorScan = now + kAnchorScanInterval;
        }

        bool hasLooseWorldInstance = false;
        {
            std::scoped_lock lock(_trackerMutex);
            hasLooseWorldInstance = _trackedAnchorCount != 0;
        }
        serviceBackpackOwnership(hasLooseWorldInstance);
        servicePullPrompt();

        const auto retrievalNeedsRealtime = [this] {
            return _retrievalBusy.load(std::memory_order_acquire) || _activeBagForm.load() != 0;
        };
        if (!settings.panelEnabled) {
            _realtimeUpdatesRequested.store(
                retrievalNeedsRealtime(),
                std::memory_order_release);
            hidePanelAndResetSpatialState();
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->menuMode != 0 || ui->itemMenuMode.load_unchecked() != 0) {
            _realtimeUpdatesRequested.store(
                retrievalNeedsRealtime(),
                std::memory_order_release);
            hidePanelAndResetSpatialState();
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerCell = player ? player->GetParentCell() : nullptr;
        auto* camera = RE::PlayerCamera::GetSingleton();
        RE::NiPointer<RE::NiNode> cameraRoot = camera ? camera->cameraRoot : nullptr;
        if (!playerCell || !cameraRoot || !finitePoint(cameraRoot->world.translate)) {
            _realtimeUpdatesRequested.store(
                retrievalNeedsRealtime(),
                std::memory_order_release);
            hidePanelAndResetSpatialState();
            return;
        }
        const RE::NiPoint3 cameraPosition = cameraRoot->world.translate;

        std::array<TrackedAnchor, kMaximumTrackedAnchors> tracked{};
        std::size_t trackedCount = 0;
        {
            std::scoped_lock lock(_trackerMutex);
            trackedCount = _trackedAnchorCount;
            std::copy_n(_trackedAnchors.begin(), trackedCount, tracked.begin());
        }

        std::array<FrameSample, kMaximumTrackedAnchors> samples{};
        std::array<anchor_policy::Candidate, kMaximumTrackedAnchors> candidates{};
        std::array<std::uint32_t, kMaximumTrackedAnchors> invalidHandles{};
        std::size_t sampleCount = 0;
        std::size_t invalidCount = 0;

        for (std::size_t i = 0; i < trackedCount; ++i) {
            auto& trackedAnchor = tracked[i];
            auto reference = trackedAnchor.handle.get();
            if (!reference || reference->IsDeleted() || reference->IsDisabled()) {
                invalidHandles[invalidCount++] = trackedAnchor.nativeHandle;
                continue;
            }
            auto* baseForm = reference->GetObjectReference();
            if (!baseForm || !isAllowedBaseForm(baseForm->GetFormID())) {
                invalidHandles[invalidCount++] = trackedAnchor.nativeHandle;
                continue;
            }

            RE::NiPointer<RE::NiAVObject> root{ reference->Get3D() };
            if (!root || !sharesActiveWorldContext(playerCell, reference->GetParentCell()) ||
                !finitePoint(root->world.translate)) {
                continue;
            }

            const float squared = distanceSquared(cameraPosition, root->world.translate);
            if (!std::isfinite(squared) || squared < 0.0f) {
                continue;
            }

            auto& sample = samples[sampleCount];
            sample.nativeHandle = trackedAnchor.nativeHandle;
            sample.candidate = { trackedAnchor.nativeHandle, squared, true };
            sample.rootWorld = copyTransform(root->world);
            candidates[sampleCount] = sample.candidate;
            ++sampleCount;
        }

        for (std::size_t i = 0; i < invalidCount; ++i) {
            removeTrackedHandle(invalidHandles[i]);
        }

        const auto selection = anchor_policy::selectStickyNearest(
            std::span<const anchor_policy::Candidate>{ candidates.data(), sampleCount },
            _activeNativeHandle.load(std::memory_order_acquire),
            kVisibleDistanceSquared);
        if (!selection) {
            _activeNativeHandle.store(0, std::memory_order_release);
            _realtimeUpdatesRequested.store(
                retrievalNeedsRealtime(),
                std::memory_order_release);
            hidePanelAndResetSpatialState();
            return;
        }
        _activeNativeHandle.store(selection.formId, std::memory_order_release);

        const FrameSample* selectedSample = nullptr;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            if (samples[i].nativeHandle == selection.formId) {
                selectedSample = &samples[i];
                break;
            }
        }
        if (!selectedSample) {
            _realtimeUpdatesRequested.store(
                retrievalNeedsRealtime(),
                std::memory_order_release);
            hidePanelAndResetSpatialState();
            return;
        }

        RE::NiPointer<RE::TESObjectREFR> backpack;
        for (std::size_t i = 0; i < trackedCount; ++i)
            if (tracked[i].nativeHandle == selection.formId) { backpack = tracked[i].handle.get(); break; }
        if (!backpack || !BackpackStorage::get().select(*backpack, _storageBaseForm)) {
            hidePanelAndResetSpatialState(); return;
        }
        const auto priorBag = _activeBagForm.exchange(backpack->formID);
        if (priorBag != backpack->formID) { _inventoryOpen = false; _inventoryModelDirty = true; }
        _realtimeUpdatesRequested = true;
        refreshInventoryModel(priorBag != backpack->formID);
        std::array<storage::Kind,3> mountKinds{};
        const auto mounts = BackpackStorage::get().mounts(mountKinds);
        _scene.update(*backpack, mounts, settings.mounts,mountKinds,elapsedSeconds);
        publishCollisionDisplays();
        const auto displayContainer=BackpackStorage::get().displayContainer();
        {
            std::scoped_lock lock(_storageMailbox);
            _displayContainer=displayContainer;
            for (unsigned i=0; i<mounts.size(); ++i) {
                _mountReferences[i]=mounts[i].id;
            }
            _warmMounts=mounts;
        }
        ZoneHint zoneHint;
        { std::scoped_lock lock(_storageMailbox); zoneHint = _zoneHint; }
        rock::api::core::Snapshot guidanceFrame;
        const bool guidanceFresh = _rock.client.core() && _rock.client.core()->getSnapshot(_rock.client.owner(), &guidanceFrame) == rock::api::Status::Ok &&
            guidanceFrame.providerReady && !guidanceFrame.menuBlocking && !guidanceFrame.configBlocking &&
            guidanceFrame.sample.frameIndex >= zoneHint.frame && guidanceFrame.sample.frameIndex - zoneHint.frame <= 3;
        if(guidanceFresh) {
            const auto held=_rock.sample(guidanceFrame);
            if(held.handsValid) {
                if(held.rightHeldFormId==backpack->formID) _panelBagRight=true;
                else if(held.leftHeldFormId==backpack->formID) _panelBagRight=false;
            }
        }
        for(unsigned i=0;i<_guides.size();++i) {
            const auto location=storage::Location(i);
            const auto region=zones::depositRegion(location);
            const auto pose=zones::guidancePose(selectedSample->rootWorld,location);
            const auto width=region.half.x*2*selectedSample->rootWorld.scale,height=region.half.y*2*selectedSample->rootWorld.scale;
            auto& guide=_guides[i]; const auto bit=1u<<i;
            if(guidanceFresh && pose && (zoneHint.guidance.visible&bit) && !(zoneHint.guidance.blocked&bit) && guide.ensureRegistered(width,height)) {
                guide.publishGuidance(zoneHint.guidance);
                (void)guide.present(*pose,width,height);
            } else guide.hide();
        }
        const auto buttonPose = panel_math::computeBackpackPanelPose(selectedSample->rootWorld,settings.buttonOffset);
        if (buttonPose && _opener.ensureRegistered(settings.buttonWidth,settings.buttonHeight))
            (void)_opener.present(*buttonPose,settings.buttonWidth,settings.buttonHeight);
        if (const auto action = _opener.takeAction(); action && _opener.accepts(*action)) handleStorageAction(*action);
        if (!_inventoryOpen) { _panel.hide(); _panelVisible = false; return; }
        if (!_panel.ensureRegistered(settings.panelPhysicalWidth, settings.panelPhysicalHeight)) { _panel.hide(); _panelVisible = false; return; }
        const auto pose = panel_math::anchoredInventoryPose(selectedSample->rootWorld, settings.panelPhysicalWidth, settings.localOffset,
            _panelBagRight,settings.panelYawDegrees);
        if (!pose || !_panel.present(*pose, settings.panelPhysicalWidth, settings.panelPhysicalHeight)) { _panelVisible = false; return; }
        const bool justShown = !_panelVisible; _panelVisible = true;
        refreshVitals(justShown);
        if (const auto action = _panel.takeAction(); action && _panel.accepts(*action)) {
            try { handleStorageAction(*action); }
            catch (const std::exception& error) { logger::error("Backpack action failed: {}", error.what()); completeItemAction("Item action failed", false); }
            catch (...) { completeItemAction("Item action failed", false); }
        }
    }
    catch (const std::exception& error) { logger::error("Backpack control task failed: {}", error.what()); }
    catch (...) { logger::error("Backpack control task failed"); }
}
