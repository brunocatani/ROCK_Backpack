#pragma once

#include "AnchorSelectionPolicy.h"
#include "BackpackConfig.h"
#include "BackpackRetrievalPolicy.h"
#include "PanelPoseMath.h"
#include "PlayerInventory.h"

#include "RE/Bethesda/BSPointerHandle.h"

#include "RockServices.h"
#include "BackpackPanel.h"
#include "GrabCommandDispatch.h"
#include "BackpackStorage.h"
#include "BackpackScene.h"
#include "StorageGesture.h"
#include "AuthoredWeaponPull.h"
#include "WeaponTransition.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace rock_backpack
{
    class BackpackRuntime final
    {
    public:
        static BackpackRuntime& get() noexcept;

        void onGameDataReady() noexcept;
        void onPreLoadGame() noexcept;
        void onGameSessionReady(bool resetTrackedState) noexcept;
        void onFrame() noexcept;

    private:
        static constexpr std::size_t kMaximumTrackedAnchors = 128;
        static constexpr std::size_t kMaximumAnchorBaseForms = 64;

        struct TrackedAnchor
        {
            RE::ObjectRefHandle handle{};
            std::uint32_t nativeHandle{ 0 };
        };

        struct FrameSample
        {
            std::uint32_t nativeHandle{ 0 };
            anchor_policy::Candidate candidate{};
            panel_math::StoredTransform rootWorld{};
        };

        enum class HapticCue : std::uint8_t
        {
            None,
            StashReady,
            ZoneEntered,
            Rejected,
            WeaponCommitted,
            ArmorCommitted,
            ItemCommitted,
            RetrievalCommitted,
        };

        // A physical back-zone pull and a panel grab share the same ROCK
        // force-grab handshake but not their side effects: only the backpack
        // pull owns backpack ownership state and anchor rescans.
        enum class RetrievalKind : std::uint8_t
        {
            BackpackPull,
            PanelGrab,
            PanelDrop,
        };

        // Where a panel-issued drop or grab should place the item.
        struct FreeHandDelivery
        {
            bool valid{ false };
            const char* reason{ "handTrackingUnavailable" };
            rock::api::Hand hand{
                rock::api::Hand::Left
            };
            retrieval_policy::Vec3 position{};
            std::uint32_t worldGeneration{ 0 };
            std::uint32_t skeletonGeneration{ 0 };
            std::uint32_t providerGeneration{ 0 };
        };

        struct RetrievalRequest
        {
            bool pending{ false };
            rock::api::Hand hand{rock::api::Hand::Left};
            retrieval_policy::Vec3 dropPosition{};
            std::uint32_t worldGeneration{ 0 };
            std::uint32_t skeletonGeneration{ 0 };
            std::uint32_t providerGeneration{ 0 };
        };

        enum class RetrievalTransactionPhase : std::uint8_t
        {
            Idle,
            AwaitReference,
            AwaitRockResult,
        };

        struct RetrievalTransaction
        {
            RetrievalTransactionPhase phase{
                RetrievalTransactionPhase::Idle
            };
            RetrievalKind kind{ RetrievalKind::BackpackPull };
            rock::api::Hand hand{
                rock::api::Hand::Left
            };
            RE::ObjectRefHandle handle{};
            std::uint64_t dispatchTicket{ 0 };
            bool admissionLogged{ false };
            bool positionedAfterLoad{ false };
            std::uint64_t positionedFrame{};
            std::chrono::steady_clock::time_point started{};
            RetrievalRequest request{};
            // Display name of a panel-grabbed row, kept only for logging.
            std::string itemName;
        };

        BackpackRuntime() = default;

        bool resolveAnchorRegistry() noexcept;
        void startScheduler() noexcept;
        void scheduleFrameTask() noexcept;
        [[nodiscard]] bool registerRockConsumer(bool logFailure) noexcept;
        static void ROCK_CALL onRockFrame(
            const rock::api::core::Snapshot* snapshot,
            void* userData) noexcept;
        void processRockFrame(
            const RockFrame& snapshot) noexcept;
        static void ROCK_CALL onRockGrabEvent(const rock::api::grab::Event*, void*) noexcept;
        void serviceBackpackReturn() noexcept;

        void processBackpackRetrievalFrame(
            const RockFrame& snapshot) noexcept;
        void resetRetrievalGesture() noexcept;
        void serviceBackpackOwnership(bool hasLooseWorldInstance) noexcept;
        void serviceBackpackRetrievalTransaction() noexcept;
        void finishRetrievalTransaction(const char* outcome) noexcept;
        void failRetrievalTransaction(const char* reason) noexcept;
        void rebuildCurrentCellAnchors() noexcept;
        void removeTrackedHandle(std::uint32_t nativeHandle) noexcept;
        void clearTrackedState() noexcept;
        [[nodiscard]] bool isAllowedBaseForm(std::uint32_t formId) const noexcept;

        void hidePanelAndResetSpatialState() noexcept;
        void refreshInventoryModel(bool force) noexcept;
        void sendInventoryModel() noexcept;
        void refreshVitals(bool force) noexcept;
        void resetInventoryModel(const char* status) noexcept;
        void completeItemAction(const char* status, bool committed);
        [[nodiscard]] FreeHandDelivery resolveFreeHandDelivery() noexcept;
        void queueHaptic(
            ui::Hand hand,
            HapticCue cue) noexcept;
        void queuePointerHaptic(HapticCue cue) noexcept;
        void serviceHaptics() noexcept;
        void processStorageFrame(const RockFrame&) noexcept;
        void protectBackpackInput(const RockFrame&) noexcept;
        void serviceStorageRequests();
        void handleStorageAction(const ui::Action&);
        void resetStorageInteraction() noexcept;
        void servicePullPrompt() noexcept;
        void publishCollisionDisplays() noexcept;
        struct VisiblePullResult { std::uint64_t display{}; bool ownsInput{},latched{}; };
        VisiblePullResult processVisibleWeaponPull(const RockFrame&,unsigned hand,bool available,bool down,
            const ui::Model*,const std::array<BackpackScene::DisplayBinding,3>&);
        void clearVisiblePullPresentation(unsigned hand,std::uint64_t frame);
        // Draw stand-ins and warm mounts (BackpackWeaponTransitions.cpp).
        void trackDrawStandIns(const RockFrame&) noexcept;
        void serviceWarmMounts(const RockFrame&) noexcept;
        void serviceStandIns(float elapsedSeconds) noexcept;
        [[nodiscard]] std::optional<panel_math::StoredTransform> heldWeaponArrival(const RockFrame&,unsigned hand,
            std::uint32_t looseReference,const RE::NiAVObject& backpackRoot) const noexcept;
        [[nodiscard]] float sceneElapsedSeconds() noexcept;

        std::array<std::uint32_t, kMaximumAnchorBaseForms> _allowedBaseFormIds{};
        std::size_t _allowedBaseFormCount{ 0 };
        std::uint32_t _backpackBaseFormId{ 0 };

        std::mutex _trackerMutex;
        std::array<TrackedAnchor, kMaximumTrackedAnchors> _trackedAnchors{};
        std::size_t _trackedAnchorCount{ 0 };
        std::atomic_bool _trackerOverflowLogged{ false };
        std::atomic_uint32_t _activeNativeHandle{ 0 };

        std::atomic_bool _gameDataReady{ false };
        std::atomic_bool _sessionActive{ false };
        std::atomic_bool _backpackFeatureEnabled{ false };
        std::atomic_bool _backpackOnBack{ false };
        std::atomic_bool _retrievalBusy{ false };
        std::atomic<rock::api::Hand> _retrievalHand{rock::api::Hand::None};
        bool _previousFeatureEnabled{ false };

        // Native inventory work stays on the serialized F4SE task path. Every
        // owner-thread-only Grab call runs through the ROCK frame callback.
        std::mutex _controlMutex;
        GrabCommandDispatch _grabDispatch;
        std::mutex _retrievalMutex;
        std::array<retrieval_policy::State, 2> _retrievalState{};
        RetrievalRequest _retrievalRequest{};
        RetrievalTransaction _retrievalTransaction{};
        bool _grabWakeRegistered{}; // ROCK owner thread only
        rock::api::Status _grabWakeStatus{ rock::api::Status::Ok };

        // The scheduler thread never touches engine or UI state. It only enqueues
        // one coalesced F4SE task. F4SE may execute those tasks on worker
        // threads; they must never call owner-thread-only ROCK operations.
        std::atomic_bool _schedulerStarted{ false };
        std::atomic_bool _realtimeUpdatesRequested{ false };
        std::atomic_bool _frameTaskScheduled{ false };
        std::atomic_bool _anchorScanRequested{ true };
        std::jthread _schedulerThread;
        std::chrono::steady_clock::time_point _nextAnchorScan{};
        BackpackConfig _config;

        RockServices _rock;
        BackpackPanel _panel;
        BackpackPanel _opener{BackpackPanel::Kind::Opener};
        BackpackPanel _pullMarker{BackpackPanel::Kind::PullPrompt,storage::Location::Interior};
        std::array<BackpackPanel,4> _guides{{
            BackpackPanel{BackpackPanel::Kind::Guidance,storage::Location::Interior},
            BackpackPanel{BackpackPanel::Kind::Guidance,storage::Location::LeftRifle},
            BackpackPanel{BackpackPanel::Kind::Guidance,storage::Location::RightRifle},
            BackpackPanel{BackpackPanel::Kind::Guidance,storage::Location::Pistol}}};
        BackpackScene _scene;
        std::uint32_t _storageBaseForm{};
        bool _inventoryOpen{};
        bool _panelBagRight{};
        std::atomic_uint32_t _inventoryFullHands{};
        std::atomic_uint32_t _activeBagForm{};
        std::atomic_uint64_t _storageEpoch{1};
        std::uint64_t _observedStorageEpoch{}, _nextClaimRestoreFrame{};
        struct StashRequest {
            std::uint32_t reference{}, bag{}, world{}, skeleton{}, provider{};
            storage::Location location{};
            rock::api::Hand hand{};
            std::uint64_t lease{};
            bool nativeEquipped{};
            // Weapon base form and where it left the hand, in backpack root
            // space, for the mount visual's arrival ease.
            std::uint32_t form{};
            std::optional<panel_math::StoredTransform> arrival;
        };
        struct ZoneGesture {
            std::uint32_t reference{}, equipped{}, bag{}, form{};
            std::uint64_t lease{}, releaseCommand{}, deadline{}, nextDiagnostic{};
            storage::Kind kind{storage::Kind::Misc};
            storage::Location location{};
            bool valid{}, down{}, releasing{}, queued{};
            std::optional<storage::Location> hover;
        };
        struct StorageClaim { std::uint32_t reference{}; std::uint64_t lease{}; };
        struct MountDraw {
            std::uint32_t bag{};
            storage::ItemId item{};
            std::uint64_t generation{};
            ui::Hand hand{};
        };
        struct WeaponDraw {
            std::uint32_t form{}, uniqueBase{}, world{}, skeleton{}, provider{};
            std::uint16_t uniqueIndex{};
            rock::api::Hand hand{rock::api::Hand::None};
            // The mount clone stands in for the weapon in the hand: ROCK is
            // asked for a holster draw that continues from the firing aim.
            bool standIn{};
        };
        // ROCK owner thread: one draw with a stand-in per hand, followed until
        // ROCK shows the real weapon or the equip ends.
        struct DrawTracking {
            std::uint64_t command{};
            std::uint32_t form{}, weaponSlot{};
            std::chrono::steady_clock::time_point revealDeadline{};
            bool active{}, succeeded{};
        };
        enum class StandInEnd : std::uint8_t { None, Release, Return };
        // Owner -> control task, per hand, under the storage mailbox.
        struct StandInFeed {
            std::optional<panel_math::StoredTransform> aimLocal; // relative to the hand's wand node
            bool shown{};
            StandInEnd end{StandInEnd::None};
        };
        struct WeaponDrawResult {
            rock::api::Hand hand{};
            bool success{};
            rock::api::Status status{rock::api::Status::Ok};
            rock::api::weapon::EquipFailure failure{};
        };
        struct ZoneHint { unsigned readyHands{}; std::uint64_t frame{}; zones::Guidance guidance{0,0,0}; };
        // Value snapshot of the left-shoulder pull prompt, owner -> game task.
        struct PullHint {
            panel_math::Vec3 eye{}, forward{}, right{};
            ui::PullPrompt prompt;
            std::uint64_t frame{};
            std::uint32_t bag{}, world{}, skeleton{}, provider{};
            bool visible{};
        };
        // The ROCK owner uses try_lock; native inventory mutations never run
        // while holding this mailbox. It contains values and immutable models.
        std::mutex _storageMailbox;
        std::shared_ptr<const ui::Model> _spatialModel;
        std::vector<std::uint32_t> _storedReferences;
        std::array<ZoneGesture, 2> _zoneGestures{}; // owner thread only
        std::array<std::atomic_uint32_t,2> _deliveries{};
        std::array<storage::DeliveryGesture,2> _deliveryGestures{};
        std::uint64_t _bagClaim{};
        std::uint32_t _claimedBag{};
        std::array<std::uint64_t,3> _mountReferences{}; // mailbox-owned copied scene identities
        std::uint32_t _displayContainer{};
        std::array<StorageClaim, 256> _storageClaims{}; // owner thread only
        std::array<std::optional<StashRequest>, 2> _stashRequests;
        ui::Hand _nativeStowFeedbackHand{ui::Hand::None}; // serialized control task
        std::array<std::uint64_t, 2> _stashAcknowledged{};
        std::optional<MountDraw> _mountDraw;
        std::optional<WeaponDraw> _weaponDraw;
        std::optional<WeaponDrawResult> _weaponDrawResult;
        std::optional<WeaponDrawResult> _weaponDrawCompletion; // ROCK callback thread only
        std::atomic_bool _weaponDrawBusy{};
        std::uint64_t _weaponDrawCommand{}; // ROCK callback thread only
        rock::api::Hand _weaponDrawHand{rock::api::Hand::None};
        ZoneHint _zoneHint;
        PullHint _pullHint;
        std::uint64_t _nextPullDiagnostic{};
        bool _bagPullReserved{}; // current owner callback, before zone routing
        std::array<BackpackScene::DisplayBinding,3> _collisionDisplays{}; // mailbox-owned identities
        std::array<DrawTracking,2> _drawTracking{}; // ROCK owner thread only
        std::array<StandInFeed,2> _standInFeeds{}; // storage mailbox
        std::array<InventoryDisplayItem,3> _warmMounts{}; // storage mailbox, published by the control task
        struct WarmState {
            std::array<InventoryDisplayItem,3> sent{};
            std::array<std::uint32_t,3> generations{};
            std::uint64_t retryFrame{};
            rock::api::Status lastStatus{rock::api::Status::Ok};
            bool valid{};
        };
        WarmState _warm; // ROCK owner thread only
        std::chrono::steady_clock::time_point _lastSceneTick{}; // control task
        struct VisiblePull {
            authored_pull::Gesture gesture;
            rock::api::storage::GripPose pose;
            bool visualOwned{},touchOwned{};
        };
        std::array<VisiblePull,2> _visiblePulls{}; // owner-frame thread only
        std::uint64_t _nextVisiblePullDiagnostic{};
        std::uint64_t _nextInteractionDiagnostic{}; // owner thread
        unsigned _pendingEntryCueHands{}; // owner thread; published with the frame's zone hint
        std::atomic_uint32_t _entryCueHands{}; // Owner publishes; game task drains.
        std::chrono::steady_clock::time_point _nextRockRetry{};
        std::atomic_uint64_t _rockOwnerToken{};
        std::atomic_uint64_t _rockFrameCallbackToken{};
        bool _panelVisible{};
        ui::Hand _actionHand{ ui::Hand::None };

        std::uint64_t _inventoryGeneration{ 0 };
        std::uint64_t _exportedInventoryGeneration{};
        std::string _inventoryStatus{ "Inventory link starting" };
        std::atomic_bool _inventoryModelDirty{ true };
        std::chrono::steady_clock::time_point _nextInventoryRefresh{};

        // Sample drifting vitals separately from inventory enumeration.
        player_inventory::Vitals _vitals{};
        bool _vitalsInitialized{ false };
        std::chrono::steady_clock::time_point _nextVitalsRefresh{};

        // Feedback is coalesced before OpenVR work on the scheduled game task.
        std::mutex _hapticMutex;
        HapticCue _pendingLeftHaptic{ HapticCue::None };
        HapticCue _pendingRightHaptic{ HapticCue::None };
        std::chrono::steady_clock::time_point _nextUiHapticAllowed{};
        std::chrono::steady_clock::time_point _nextStashHaptic{};
        std::chrono::steady_clock::time_point _hapticActiveUntil{}; // Existing task pumps the entry pulse envelope.
    };
}
