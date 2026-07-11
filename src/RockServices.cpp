#include "RockServices.h"
#include "BackpackGripPoses.h"
#include "BackpackInteractionPolicy.h"
#include "NativeInventoryAdmission.h"
#include <Windows.h>
#include <spdlog/spdlog.h>

namespace rock_backpack
{
    bool RockServices::connect() noexcept
    {
        using namespace rock::api;
        const auto module = GetModuleHandleW(L"ROCK.dll");
        if (interaction_policy::runtime(module != nullptr, GetModuleHandleW(L"ROCK_V2.dll") != nullptr) !=
            interaction_policy::Runtime::Regular) return false;
        if (client.owner() && grab && hands && input && weapon && storage && animation && authoredTouch) return true;
        return bind(reinterpret_cast<QueryInterfaceV1>(GetProcAddress(module, kQueryExportName)));
    }

    bool RockServices::bind(rock::api::QueryInterfaceV1 query) noexcept
    {
        using namespace rock::api;
        if (client.owner()) {
            if (grab && hands && input && weapon && storage && animation && authoredTouch) return true;
            native_admission::unbindRock();
            if (client.close() != Status::Ok) return false;
        }
        grab = nullptr; hands = nullptr; input = nullptr; weapon = nullptr;
        storage = nullptr; animation = nullptr; authoredTouch = nullptr;
        registeredPoseForm = 0;
        lastPoseResult = Status::NotReady;
        if (client.connect(query, "ROCK Backpack") != Status::Ok) return false;
        constexpr auto bits = [](auto... values) { return (static_cast<std::uint32_t>(values) | ...); };
        const core::Api* callbacks{};
        if (client.acquire(bits(core::Permission::Read, core::Permission::Callbacks), callbacks) != Status::Ok ||
            client.acquire(bits(grab::Permission::Read, grab::Permission::Write), grab) != Status::Ok ||
            client.acquire(bits(hands::Permission::Read), hands) != Status::Ok ||
            client.acquire(bits(input::Permission::Read, input::Permission::Write), input) != Status::Ok ||
            client.acquire(bits(animation::Permission::Write), animation) != Status::Ok ||
            client.acquire(bits(touch::Permission::Write), authoredTouch) != Status::Ok ||
            client.acquire(bits(weapon::Permission::Read, weapon::Permission::Write), weapon) != Status::Ok ||
            client.acquire(bits(rock::api::storage::Permission::Read, rock::api::storage::Permission::Write, rock::api::storage::Permission::Callbacks), storage) != Status::Ok) {
            spdlog::error("ROCK Backpack requires ROCK's Core, Grab, Hands, Input, Animation, Touch, Weapon and Storage 2.0 interfaces.");
            grab = nullptr; hands = nullptr; input = nullptr; weapon = nullptr;
            storage = nullptr; animation = nullptr; authoredTouch = nullptr;
            const auto result = client.close();
            if (result != Status::Ok)
                spdlog::error("ROCK Backpack provider cleanup failed: {}", static_cast<unsigned>(result));
            return false;
        }
        native_admission::bindRock(weapon, client.owner());
        spdlog::info("ROCK Backpack bound ROCK's Core, Grab, Hands, Input, Animation, Touch, Weapon and Storage interfaces.");
        return true;
    }

    bool RockServices::ensureBackpackPoses(std::uint32_t baseFormId) noexcept
    {
        using rock::api::Status;
        if (!baseFormId || !client.owner() || !grab) return false;
        if (registeredPoseForm == baseFormId) return true;
        auto poses = kBackpackGripPoses;
        poses.baseFormId = baseFormId;
        const auto result = grab->setDefaultGrabPoses(client.owner(), &poses);
        if (result == Status::Ok) {
            registeredPoseForm = baseFormId;
            spdlog::info("ROCK Backpack registered captured left/right grip defaults for {:08X}.", baseFormId);
        } else if (result != lastPoseResult) {
            spdlog::error("ROCK Backpack grip defaults rejected for {:08X}: {}", baseFormId, static_cast<unsigned>(result));
        }
        lastPoseResult = result;
        return result == Status::Ok;
    }

    RockFrame RockServices::sample(const rock::api::core::Snapshot& core) const noexcept
    {
        using namespace rock::api;
        RockFrame result{};
        static_cast<core::Snapshot&>(result) = core;
        if (!client.owner() || !hands || !grab || !core.providerReady ||
            !core.frikSkeletonReady || core.menuBlocking || core.configBlocking) return result;

        const auto matches = [&core](const auto& value) {
            return value.frameIndex == core.sample.frameIndex && value.worldGeneration == core.sample.worldGeneration &&
                value.skeletonGeneration == core.sample.skeletonGeneration && value.providerGeneration == core.sample.providerGeneration;
        };
        hands::HandFrame left{}, right{};
        grab::HandInteractionState leftState{}, rightState{};
        constexpr auto validPose = static_cast<std::uint32_t>(hands::HandFrameFlag::Valid);
        constexpr auto validState = static_cast<std::uint32_t>(grab::HandInteractionFlag::Valid);
        if (hands->getHandFrame(client.owner(), Hand::Left, &left) != Status::Ok ||
            hands->getHandFrame(client.owner(), Hand::Right, &right) != Status::Ok ||
            grab->getHandInteractionState(client.owner(), Hand::Left, &leftState) != Status::Ok ||
            grab->getHandInteractionState(client.owner(), Hand::Right, &rightState) != Status::Ok ||
            !matches(left.sample) || !matches(right.sample) || !matches(leftState) || !matches(rightState) ||
            !(left.flags & validPose) || !(right.flags & validPose) ||
            !(leftState.flags & validState) || !(rightState.flags & validState)) return result;

        const auto occupied = [](const grab::HandInteractionState& state) {
            using Flag = grab::HandInteractionFlag;
            constexpr auto flags = static_cast<unsigned>(Flag::FiringGrip) |
                static_cast<unsigned>(Flag::PartGrip) | static_cast<unsigned>(Flag::PartCarry) |
                static_cast<unsigned>(Flag::TouchGrab) | static_cast<unsigned>(Flag::NativeWeaponCarry) |
                static_cast<unsigned>(Flag::RockGripActive) | static_cast<unsigned>(Flag::TransitionSuppressed);
            return (state.phase != grab::HandInteractionPhase::Idle &&
                    state.phase != grab::HandInteractionPhase::Touching &&
                    state.phase != grab::HandInteractionPhase::Selecting) ||
                state.heldBodyCount != 0 || (state.flags & flags) != 0;
        };
        result.handsValid = true;
        result.handStates = {rightState, leftState};
        // Slot 0 is the primary weapon, the only one ROCK publishes today.
        weapon::WeaponState equipped{};
        if (weapon && weapon->getWeaponState(client.owner(), 0, &equipped) == Status::Ok && matches(equipped.sample))
            result.equippedWeapon = equipped.weaponFormId;
        result.leftHandTransform = left.transform;
        result.rightHandTransform = right.transform;
        result.leftHolding = occupied(leftState);
        result.rightHolding = occupied(rightState);
        result.leftHeldFormId = result.leftHolding ? leftState.targetFormId : 0;
        result.rightHeldFormId = result.rightHolding ? rightState.targetFormId : 0;
        result.headValid = hands->getHeadPose(client.owner(), &result.head) == Status::Ok &&
            (result.head.flags & static_cast<std::uint32_t>(hands::HeadPoseFlag::TransformValid)) && matches(result.head.sample);
        return result;
    }
}
