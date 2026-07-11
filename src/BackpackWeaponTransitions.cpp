#include "PCH.h"
#include "BackpackRuntime.h"
#include "f4vr/PlayerNodes.h"

// Stash and draw transitions of mounted weapons, modelled on VirtualHolsters.
// The ROCK owner callback reads ROCK (firing aim, equip results, weapon
// visibility, warm weapons) and publishes plain values; the control task owns
// every scene clone and applies them (BackpackScene).
namespace rock_backpack
{
    namespace
    {
        namespace api = rock::api;
        using Clock = std::chrono::steady_clock;

        panel_math::StoredTransform stored(const RE::NiTransform& value)
        {
            panel_math::StoredTransform out;
            for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) out.rotation[r * 3 + c] = value.rotate.entry[r][c];
            out.translation = {value.translate.x, value.translate.y, value.translate.z}; out.scale = value.scale;
            return out;
        }
        // ROCK transforms are row-major copies of the engine matrix.
        panel_math::StoredTransform stored(const api::Transform& value)
        {
            panel_math::StoredTransform out;
            std::copy_n(value.rotate, 9, out.rotation.begin());
            out.translation = {value.translate[0], value.translate[1], value.translate[2]}; out.scale = value.scale;
            return out;
        }
        RE::NiNode* wandNode(unsigned hand)
        { return hand ? f4cf::f4vr::getLeftHandNode() : f4cf::f4vr::getRightHandNode(); }
    }

    float BackpackRuntime::sceneElapsedSeconds() noexcept
    {
        const auto now = Clock::now();
        const auto previous = std::exchange(_lastSceneTick, now);
        if (previous == Clock::time_point{}) return 0.f;
        const std::chrono::duration<float> elapsed = now - previous;
        return std::clamp(elapsed.count(), 0.f, .1f);
    }

    std::optional<panel_math::StoredTransform> BackpackRuntime::heldWeaponArrival(const RockFrame& frame, unsigned,
        std::uint32_t looseReference, const RE::NiAVObject& backpackRoot) const noexcept
    {
        std::optional<panel_math::StoredTransform> world;
        if (looseReference) {
            auto* reference = RE::TESForm::GetFormByID<RE::TESObjectREFR>(looseReference);
            const auto* model = reference && !reference->IsDeleted() ? reference->Get3D() : nullptr;
            if (model) world = stored(model->world);
        } else if (_rock.weapon && frame.equippedWeapon) {
            // The equipped stash takes slot 0's weapon (frame.equippedWeapon).
            api::weapon::Grip grip;
            if (_rock.weapon->getGrip(_rock.client.owner(), 0, &grip) == api::Status::Ok &&
                (grip.flags & unsigned(api::weapon::GripFlag::WeaponWorldValid)) && grip.weaponFormId == frame.equippedWeapon)
                world = stored(grip.weaponWorld);
        }
        if (!world || !std::isfinite(backpackRoot.world.scale) || backpackRoot.world.scale <= .0001f) return {};
        return weapon_transition::localPose(stored(backpackRoot.world), *world);
    }

    void BackpackRuntime::trackDrawStandIns(const RockFrame& frame) noexcept
    {
        if (!_rock.weapon || !_rock.hands) return;
        if (!_drawTracking[0].active && !_drawTracking[1].active) return;
        const auto owner = _rock.client.owner();
        const auto now = Clock::now();
        const auto shown = [&](std::uint32_t slot, std::uint32_t form) {
            api::weapon::WeaponState state;
            if (slot >= api::kMaxWeaponSlots || !form || _rock.weapon->getWeaponState(owner, slot, &state) != api::Status::Ok ||
                state.weaponFormId != form) return false;
            // Slot 0: ROCK revealed the native model. Slot 1: ROCK presents its
            // own model in the hand without a draw wait.
            const auto flag = slot == 0 ? api::weapon::WeaponStateFlag::NativeRenderable : api::weapon::WeaponStateFlag::WeaponPresent;
            return (state.flags & unsigned(flag)) != 0;
        };
        api::hands::Roles roles;
        const bool rolesValid = _rock.hands->getRoles(owner, &roles) == api::Status::Ok;
        std::array<StandInFeed, 2> feeds{};
        for (unsigned h = 0; h < 2; ++h) {
            auto& track = _drawTracking[h];
            if (!track.active) continue;
            auto& feed = feeds[h];
            const auto hand = h ? api::Hand::Left : api::Hand::Right;
            api::Transform aim;
            if (auto* wand = frame.handsValid ? wandNode(h) : nullptr;
                wand && std::isfinite(wand->world.scale) && wand->world.scale > .0001f &&
                _rock.weapon->getFiringAimWorld(owner, hand, &aim) == api::Status::Ok)
                feed.aimLocal = weapon_transition::localPose(stored(wand->world), stored(aim));
            if (!track.succeeded) {
                api::weapon::EquipResult result;
                using State = api::weapon::EquipState;
                const auto status = track.command ? _rock.weapon->getInventoryEquipResult(owner, track.command, &result) : api::Status::RequestNotFound;
                if (status != api::Status::Ok || result.state == State::Failed || result.state == State::Cancelled) {
                    feed.end = StandInEnd::Return; continue;
                }
                if (result.state == State::Succeeded) {
                    track.succeeded = true; track.weaponSlot = result.weaponSlot;
                    track.revealDeadline = now + std::chrono::milliseconds(int(weapon_transition::kRevealWaitSeconds * 1000));
                }
            }
            // Before the equip result names the slot, the slot the hand holds.
            std::uint32_t slot = track.weaponSlot;
            if (!track.succeeded) {
                const auto& role = h ? roles.left : roles.right;
                slot = rolesValid && role.role != api::hands::HandRole::None ? role.weaponSlot : api::kMaxWeaponSlots;
            }
            feed.shown = shown(slot, track.form);
            if (track.succeeded && (feed.shown || now >= track.revealDeadline)) {
                if (!feed.shown) logger::info("Backpack draw stand-in in hand {} removed before ROCK showed the weapon", h);
                feed.end = StandInEnd::Release;
            }
        }
        std::unique_lock lock(_storageMailbox, std::try_to_lock);
        if (!lock) return; // ends are recomputed next frame from ROCK's retained results
        for (unsigned h = 0; h < 2; ++h) {
            if (!_drawTracking[h].active) continue;
            auto& current = _standInFeeds[h];
            const auto end = current.end != StandInEnd::None ? current.end : feeds[h].end;
            current = feeds[h]; current.end = end;
            if (feeds[h].end != StandInEnd::None) _drawTracking[h] = {};
        }
        lock.unlock();
        scheduleFrameTask();
    }

    void BackpackRuntime::serviceWarmMounts(const RockFrame& frame) noexcept
    {
        if (!_rock.weapon || !frame.providerReady || frame.menuBlocking || frame.configBlocking) return;
        std::array<InventoryDisplayItem, 3> mounts;
        { std::unique_lock lock(_storageMailbox, std::try_to_lock); if (!lock) return; mounts = _warmMounts; }
        const std::array<std::uint32_t, 3> generations{frame.sample.worldGeneration, frame.sample.skeletonGeneration, frame.sample.providerGeneration};
        const auto same = [](const InventoryDisplayItem& a, const InventoryDisplayItem& b) {
            return a.id == b.id && a.form == b.form && a.index == b.index && a.stackWitness == b.stackWitness && a.instanceWitness == b.instanceWitness;
        };
        if (_warm.valid && generations == _warm.generations && std::equal(mounts.begin(), mounts.end(), _warm.sent.begin(), same)) return;
        if (frame.sample.frameIndex < _warm.retryFrame) return;
        // ROCK keeps each mounted firearm's animations loaded, so a draw off
        // the backpack never waits on the engine's first-use load.
        const auto owner = _rock.client.owner();
        std::array<api::weapon::InventoryWeapon, 3> items{};
        std::uint32_t count{};
        auto status = api::Status::Ok;
        for (const auto& mount : mounts) if (mount) {
            api::weapon::InventoryWeapon item;
            status = _rock.weapon->captureInventoryWeapon(owner, mount.form, mount.index, &item);
            if (status == api::Status::Ok && (item.stackKey != mount.stackWitness || item.instanceKey != mount.instanceWitness))
                status = api::Status::TargetUnavailable;
            if (status != api::Status::Ok) break;
            items[count++] = item;
        }
        if (status == api::Status::Ok) status = _rock.weapon->setWarmInventoryWeapons(owner, items.data(), count);
        if (status == api::Status::Ok) {
            if (count) logger::info("Backpack keeps {} mounted weapon(s) ready to draw", count);
            _warm = {mounts, generations, 0, api::Status::Ok, true};
            return;
        }
        _warm.retryFrame = frame.sample.frameIndex + 90;
        if (status != _warm.lastStatus && status != api::Status::NotReady)
            logger::warn("Backpack mounted weapons not kept ready to draw: status={}", unsigned(status));
        _warm.lastStatus = status;
    }

    void BackpackRuntime::serviceStandIns(float elapsedSeconds) noexcept
    {
        try {
            std::array<StandInFeed, 2> feeds;
            {
                std::scoped_lock lock(_storageMailbox);
                feeds = _standInFeeds;
                for (auto& feed : _standInFeeds) feed.end = StandInEnd::None;
            }
            for (unsigned h = 0; h < feeds.size(); ++h) {
                if (!_scene.standInActive(h)) continue;
                const auto& feed = feeds[h];
                // A draw that never reports back puts its stand-in away. The
                // mount takes it back only if the weapon is listed there again.
                if (feed.end != StandInEnd::None || _scene.standInSeconds(h) > weapon_transition::kStandInLimitSeconds) {
                    if (feed.end == StandInEnd::None) logger::warn("Backpack draw stand-in in hand {} timed out without a ROCK result", h);
                    _scene.endStandIn(h, feed.end != StandInEnd::Release);
                    continue;
                }
                _scene.updateStandIn(h, feed.aimLocal, feed.shown, elapsedSeconds);
            }
        } catch (...) { logger::error("Backpack draw stand-in update failed"); }
    }
}
