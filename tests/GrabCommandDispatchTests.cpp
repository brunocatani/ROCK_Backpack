#include "GrabCommandDispatch.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
    using namespace rock::api;
    using Dispatch = rock_backpack::GrabCommandDispatch;
    const auto ownerThread = std::this_thread::get_id();
    std::atomic<unsigned> requests{}, queries{}, cancellations{}, wrongThread{};
    Status admission = Status::RequestQueued;
    grab::InteractionCommandState completion = grab::InteractionCommandState::Queued;
    grab::ForceGrabRequest received;
    constexpr OwnerToken owner = 27;
    constexpr std::uint64_t command = 91;
    bool mismatchedReply{};

    void require(bool condition, const char* message)
    { if (!condition) { std::cerr << message << '\n'; std::exit(1); } }
    bool onOwner()
    {
        if (std::this_thread::get_id() == ownerThread) return true;
        ++wrongThread; return false;
    }
    Status ROCK_CALL request(OwnerToken token, const grab::ForceGrabRequest* value, std::uint64_t* id) noexcept
    {
        if (!onOwner()) return Status::WrongThread;
        require(token == owner, "request owner changed");
        ++requests; received = *value;
        if (admission == Status::RequestQueued) *id = command;
        return admission;
    }
    Status ROCK_CALL query(OwnerToken token, std::uint64_t id, grab::InteractionCommandResult* result) noexcept
    {
        if (!onOwner()) return Status::WrongThread;
        require(token == owner && id == command, "query identity changed");
        ++queries;
        *result = {};
        result->ownerToken = owner; result->commandId = mismatchedReply ? command + 1 : command;
        result->kind = grab::InteractionCommandKind::ForceGrab;
        result->state = completion; result->targetFormId = received.targetFormId;
        return Status::Ok;
    }
    Status ROCK_CALL cancel(OwnerToken token, std::uint64_t id) noexcept
    {
        if (!onOwner()) return Status::WrongThread;
        require(token == owner && id == command, "cancel identity changed");
        ++cancellations;
        if (completion == grab::InteractionCommandState::Succeeded) return Status::RequestNotFound;
        completion = grab::InteractionCommandState::Cancelled;
        return Status::Ok;
    }
    const grab::Api api{ .requestForceGrab = request, .getInteractionCommandResult = query,
        .cancelInteractionCommand = cancel };
}

int main()
{
    Dispatch dispatch;
    grab::ForceGrabRequest item;
    item.hand = Hand::Left; item.targetFormId = 0xFF000803;
    item.worldGeneration = 5; item.skeletonGeneration = 6; item.providerGeneration = 7; item.maxDistanceGame = 32;
    std::uint64_t ticket{};
    {
        std::jthread worker([&] {
            ticket = dispatch.submit(item);
            const auto reply = dispatch.read(ticket);
            require(ticket && reply && reply->phase == Dispatch::Phase::Requested, "worker must publish a pending request");
            require(requests == 0 && queries == 0, "worker submission or polling entered the owner-only API");
        });
    }
    require(dispatch.submit(item) == 0, "a second transfer must not overwrite the active one");
    require(dispatch.pump(api, owner, 123), "owner callback should admit the request");
    require(requests == 1 && queries == 1 && wrongThread == 0, "owner callback must exclusively perform admission and polling");
    require(received.hand == Hand::Left && received.targetFormId == item.targetFormId &&
        received.worldGeneration == 5 && received.skeletonGeneration == 6 && received.providerGeneration == 7 && received.maxDistanceGame == 32,
        "dispatch changed the exact reference, physical hand, generation or range");
    auto reply = dispatch.read(ticket);
    require(reply && reply->phase == Dispatch::Phase::Running && reply->commandId == command && reply->executorThread == 123,
        "request ticket must retain its provider command and executor identity");
    completion = grab::InteractionCommandState::Succeeded;
    dispatch.pump(api, owner, 123);
    {
        std::jthread worker([&] {
            const auto terminal = dispatch.read(ticket);
            require(terminal && terminal->succeeded && !terminal->mayReturnToInventory, "success must not permit inventory rollback");
            dispatch.acknowledge(ticket);
        });
    }
    require(!dispatch.read(ticket), "acknowledged ticket must retire");

    completion = grab::InteractionCommandState::Queued;
    const auto cancelled = dispatch.submit(item);
    dispatch.pump(api, owner, 123);
    {
        std::jthread worker([&] { dispatch.cancel(cancelled); });
    }
    require(cancellations == 0, "worker cancellation must not enter the provider");
    reply = dispatch.read(cancelled);
    require(reply && !reply->mayReturnToInventory, "pending cancellation cannot authorize native rollback");
    dispatch.pump(api, owner, 123);
    reply = dispatch.read(cancelled);
    require(cancellations == 1 && reply && reply->phase == Dispatch::Phase::Finished && reply->mayReturnToInventory,
        "only completed owner-thread cancellation permits rollback");
    dispatch.acknowledge(cancelled);

    const auto notAdmitted = dispatch.submit(item);
    const auto before = requests.load();
    { std::jthread worker([&] { dispatch.cancel(notAdmitted, true); }); }
    dispatch.pump(api, owner, 123);
    require(requests == before && !dispatch.read(notAdmitted), "a load must discard unadmitted requests without grabbing later");

    completion = grab::InteractionCommandState::Queued;
    const auto abandoned = dispatch.submit(item);
    dispatch.pump(api, owner, 123);
    { std::jthread worker([&] { dispatch.cancel(abandoned, true); }); }
    require(dispatch.submit(item) == 0, "a new session must wait for the old provider command to retire");
    dispatch.pump(api, owner, 123);
    require(!dispatch.read(abandoned) && cancellations == 2, "abandoned command must drain on the owner callback");

    admission = Status::HandBusy;
    const auto denied = dispatch.submit(item);
    dispatch.pump(api, owner, 123);
    reply = dispatch.read(denied);
    require(reply && reply->admission == Status::HandBusy && reply->mayReturnToInventory && !reply->succeeded,
        "rejected admission must preserve safe rollback");
    dispatch.acknowledge(denied);

    admission = Status::RequestQueued; mismatchedReply = true;
    const auto mismatch = dispatch.submit(item);
    dispatch.pump(api, owner, 123);
    reply = dispatch.read(mismatch);
    require(reply && reply->query == Status::InvalidArgument && !reply->mayReturnToInventory && !reply->succeeded,
        "mismatched provider reply must not authorize success or rollback");
    require(wrongThread == 0, "a provider call escaped the owner thread");
    dispatch.acknowledge(mismatch); mismatchedReply = false;
    admission = Status::NotReady;
    const auto preparing = dispatch.submit(item);
    const auto queryBefore = queries.load();
    for (unsigned i = 0; i < 10; ++i) dispatch.pump(api, owner, 123, request);
    reply = dispatch.read(preparing);
    require(reply && reply->phase == Dispatch::Phase::Requested && !reply->commandId && queries == queryBefore,
        "authored grip preparation must wait before command admission without polling an invented command");
    dispatch.cancel(preparing);
    reply = dispatch.read(preparing);
    require(reply && reply->mayReturnToInventory && reply->phase == Dispatch::Phase::Finished,
        "cancel during preparation must safely return the exact stored reference");
    dispatch.acknowledge(preparing);
    const auto retry = dispatch.submit(item);
    dispatch.pump(api, owner, 123, request);
    admission = Status::RequestQueued; completion = grab::InteractionCommandState::Succeeded;
    dispatch.pump(api, owner, 123, request);
    require(dispatch.read(retry)->succeeded, "prepared grip must eventually deliver normally");
    std::cout << "Backpack worker/owner dispatch tests passed\n";
}
