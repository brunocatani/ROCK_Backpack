#pragma once

#include <ROCK/Grab.h>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace rock_backpack
{
    // One inventory transfer at a time. F4SE tasks exchange copied values here;
    // only pump(), called by ROCK's Core frame callback, may enter the Grab API.
    // No engine pointer, inventory stack or native reference crosses this boundary.
    class GrabCommandDispatch
    {
    public:
        enum class Phase { Requested, Running, Finished };
        struct Reply
        {
            std::uint64_t ticket{}, commandId{};
            Phase phase{ Phase::Requested };
            rock::api::Status admission{ rock::api::Status::NotReady };
            rock::api::Status query{ rock::api::Status::NotReady };
            rock::api::grab::InteractionCommandResult result{};
            std::uint32_t executorThread{};
            bool succeeded{}, mayReturnToInventory{};
        };

        std::uint64_t submit(const rock::api::grab::ForceGrabRequest& request)
        {
            std::scoped_lock lock(_mutex);
            if (_reply || _nextTicket == (std::numeric_limits<std::uint64_t>::max)()) return 0;
            _reply = Reply{ .ticket = ++_nextTicket };
            _request = request;
            _cancel = _cancelIssued = _abandoned = false;
            return _reply->ticket;
        }

        std::optional<Reply> read(std::uint64_t ticket) const
        {
            std::scoped_lock lock(_mutex);
            return _reply && _reply->ticket == ticket ? _reply : std::nullopt;
        }

        void cancel(std::uint64_t ticket, bool abandon = false)
        {
            std::scoped_lock lock(_mutex);
            if (!_reply || _reply->ticket != ticket) return;
            _cancel = true;
            _abandoned = abandon;
            // No API request exists yet, so cancellation requires no owner call.
            if (_reply->phase == Phase::Requested) {
                _reply->mayReturnToInventory = true;
                _reply->result.state = rock::api::grab::InteractionCommandState::Cancelled;
                finish();
            } else if (_reply->phase == Phase::Finished && _abandoned) _reply.reset();
        }

        void acknowledge(std::uint64_t ticket)
        {
            std::scoped_lock lock(_mutex);
            if (_reply && _reply->ticket == ticket && _reply->phase == Phase::Finished) _reply.reset();
        }

        // Nonblocking on the provider frame path. API calls remain in this
        // method, including result polling and cancellation, which are also
        // restricted to the owner thread by the published modular contract.
        using Submit = rock::api::Status(ROCK_CALL*)(rock::api::OwnerToken, const rock::api::grab::ForceGrabRequest*, std::uint64_t*) noexcept;
        bool pump(const rock::api::grab::Api& api, rock::api::OwnerToken owner, std::uint32_t thread, Submit submit = nullptr)
        {
            using namespace rock::api;
            std::unique_lock lock(_mutex, std::try_to_lock);
            if (!lock || !_reply || _reply->phase == Phase::Finished) return false;
            _reply->executorThread = thread;
            bool changed = false;
            if (_reply->phase == Phase::Requested) {
                if (_cancel) { _reply->mayReturnToInventory = true; finish(); return true; }
                _reply->admission = (submit ? submit : api.requestForceGrab)(owner, &_request, &_reply->commandId);
                if (submit && _reply->admission == Status::NotReady) return false;
                changed = true;
                if (_reply->admission != Status::RequestQueued) {
                    _reply->succeeded = _reply->admission == Status::ObjectAlreadyOwned;
                    _reply->mayReturnToInventory = !_reply->succeeded;
                    finish();
                    return true;
                }
                if (!_reply->commandId) {
                    _reply->query = Status::InternalError;
                    finish();
                    return true;
                }
                _reply->phase = Phase::Running;
            }
            Status cancellation = Status::NotReady;
            if (_cancel && !_cancelIssued) {
                cancellation = api.cancelInteractionCommand(owner, _reply->commandId);
                _cancelIssued = true;
            }
            _reply->query = api.getInteractionCommandResult(owner, _reply->commandId, &_reply->result);
            if (_reply->query == Status::RequestNotFound && !_cancel) return changed;
            if (_reply->query != Status::Ok) {
                // Only an acknowledged cancellation proves a reference is safe
                // to re-activate when the provider no longer has a result.
                _reply->mayReturnToInventory = cancellation == Status::Ok;
                finish();
                return true;
            }
            const auto& result = _reply->result;
            if (result.ownerToken != owner || result.commandId != _reply->commandId ||
                result.kind != grab::InteractionCommandKind::ForceGrab) {
                _reply->query = Status::InvalidArgument;
                finish();
                return true;
            }
            if (result.state == grab::InteractionCommandState::Queued ||
                result.state == grab::InteractionCommandState::Unknown) return changed;
            _reply->succeeded = result.state == grab::InteractionCommandState::Succeeded ||
                result.failure == grab::InteractionFailure::TargetAlreadyOwned;
            _reply->mayReturnToInventory = !_reply->succeeded &&
                (result.state == grab::InteractionCommandState::Rejected || result.state == grab::InteractionCommandState::Cancelled);
            finish();
            return true;
        }

    private:
        void finish()
        {
            _reply->phase = Phase::Finished;
            if (_abandoned) _reply.reset();
        }
        mutable std::mutex _mutex;
        std::optional<Reply> _reply;
        rock::api::grab::ForceGrabRequest _request{};
        std::uint64_t _nextTicket{};
        bool _cancel{}, _cancelIssued{}, _abandoned{};
    };
}
