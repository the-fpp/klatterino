// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleModeration.hpp"

#include <gtest/gtest.h>

namespace chatterino {
namespace {

using namespace rumble;

ModerationIdentity id(const char *value)
{
    auto parsed = ModerationIdentity::fromProvider(QString::fromUtf8(value));
    EXPECT_TRUE(parsed.has_value());
    return std::move(*parsed);
}

ModerationScope scope(const char *account = "account-01",
                      const char *channel = "channel-01")
{
    return {id(account), id(channel)};
}

TEST(RumbleModerationIdentity, PreservesOpaqueValuesAndRejectsUnsafeForms)
{
    auto large = ModerationIdentity::fromProvider("9007199254740993");
    ASSERT_TRUE(large);
    EXPECT_EQ(large->value(), "9007199254740993");
    EXPECT_FALSE(ModerationIdentity::fromProvider(""));
    EXPECT_FALSE(ModerationIdentity::fromProvider(" padded "));
    EXPECT_FALSE(ModerationIdentity::fromProvider(
        QString::fromLatin1("a\0b", 3)));
}

TEST(RumbleModerationReducer, AppliesInboundStateAndIsIdempotent)
{
    ModerationState state(scope());
    const ModerationEvent roles{0, ModerationRoleSnapshot{
                                    scope(), {ModerationRole::Moderator}}};
    EXPECT_EQ(state.apply(roles), ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply(roles), ModerationApplyResult::Stale);

    const ModerationEvent deleted{1, ModerationMessageDeleted{
                                         scope(),
                                         {id("message-9007199254740993")}}};
    EXPECT_EQ(state.apply(deleted), ModerationApplyResult::Applied);
    EXPECT_TRUE(
        state.deletedMessageIds().contains("message-9007199254740993"));

    const ModerationEvent pinned{
        2, ModerationPinChanged{scope(), id("message-2")}};
    EXPECT_EQ(state.apply(pinned), ModerationApplyResult::Applied);
    ASSERT_TRUE(state.pinnedMessageId());
    EXPECT_EQ(*state.pinnedMessageId(), "message-2");

    const ModerationEvent muted{3,
                                ModerationMuteChanged{scope(), id("user-2"),
                                                      true}};
    EXPECT_EQ(state.apply(muted), ModerationApplyResult::Applied);
    EXPECT_TRUE(state.isMuted(id("user-2")));
    EXPECT_EQ(state.apply({4,
                           ModerationMuteChanged{scope(), id("user-2"),
                                                 false}}),
              ModerationApplyResult::Applied);
    EXPECT_FALSE(state.isMuted(id("user-2")));
}

TEST(RumbleModerationReducer, RejectsStaleAndCrossScopeEventsWithoutMutation)
{
    ModerationState state(scope());
    EXPECT_EQ(state.apply({10,
                           ModerationPinChanged{scope(), id("new")}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({9,
                           ModerationPinChanged{scope(), id("old")}}),
              ModerationApplyResult::Stale);
    EXPECT_EQ(state.apply({11,
                           ModerationPinChanged{scope("account-02"),
                                                id("other")}}),
              ModerationApplyResult::WrongScope);
    EXPECT_EQ(state.apply({12,
                           ModerationPinChanged{
                               scope("account-01", "channel-02"),
                               id("other-channel")}}),
              ModerationApplyResult::WrongScope);
    ASSERT_TRUE(state.pinnedMessageId());
    EXPECT_EQ(*state.pinnedMessageId(), "new");
}

TEST(RumbleModerationReducer, BoundsGrowingStateAndSupportsUnpin)
{
    ModerationState state(scope(), 2);
    EXPECT_EQ(state.apply({1, ModerationMessageDeleted{
                                 scope(), {id("d1"), id("d2"), id("d3")}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.deletedMessageIds().size(), 2);
    EXPECT_FALSE(state.deletedMessageIds().contains("d1"));
    EXPECT_EQ(state.apply({2, ModerationMessageDeleted{scope(), {id("d1")}}}),
              ModerationApplyResult::Duplicate);
    EXPECT_FALSE(state.deletedMessageIds().contains("d1"));
    EXPECT_EQ(state.apply({3, ModerationMuteChanged{scope(), id("u1"), true}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({4, ModerationMuteChanged{scope(), id("u2"), true}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({5, ModerationMuteChanged{scope(), id("u3"), true}}),
              ModerationApplyResult::Applied);
    EXPECT_FALSE(state.isMuted(id("u1")));
    EXPECT_TRUE(state.isMuted(id("u2")));
    EXPECT_TRUE(state.isMuted(id("u3")));
    EXPECT_EQ(state.apply({6, ModerationPinChanged{scope(), id("pin")}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({7, ModerationPinChanged{scope(), std::nullopt}}),
              ModerationApplyResult::Applied);
    EXPECT_FALSE(state.pinnedMessageId());
    EXPECT_EQ(state.apply({8, ModerationPinChanged{scope(), id("pin")}}),
              ModerationApplyResult::Applied);
    ASSERT_TRUE(state.pinnedMessageId());
    EXPECT_EQ(*state.pinnedMessageId(), "pin");
    EXPECT_EQ(state.apply(
                  {9, ModerationMuteChanged{scope(), id("u2"), false}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply(
                  {10, ModerationMuteChanged{scope(), id("u2"), true}}),
              ModerationApplyResult::Applied);
    EXPECT_TRUE(state.isMuted(id("u2")));
}

TEST(RumbleModerationPipeline, AdaptsAcceptedStateOperationsWithoutWireFields)
{
    ModerationState state(scope());
    EventParser parser;
    EventState eventState;
    auto parsed = parser.parse(
        R"({"type":"delete_messages","data":{"message_ids":["message-1","message-2"]}})");
    ASSERT_TRUE(parsed.event);
    auto processed = eventState.process(*parsed.event, "stream-1");
    ASSERT_EQ(processed.operations.size(), 1);
    auto event = moderationEventFromStateOperation(
        processed.operations.front(), scope(), 0);
    ASSERT_TRUE(event);
    EXPECT_EQ(state.apply(*event), ModerationApplyResult::Applied);
    EXPECT_EQ(state.deletedMessageIds().size(), 2);

    parsed = parser.parse(
        R"({"type":"pin_message","data":{"message":{"id":"pin","user_id":"u","channel_id":"c","text":"pinned","created_on":"2026-01-01T00:00:00Z"}}})");
    ASSERT_TRUE(parsed.event);
    processed = eventState.process(*parsed.event, "stream-1");
    ASSERT_EQ(processed.operations.size(), 1);
    event = moderationEventFromStateOperation(processed.operations.front(),
                                              scope(), 1);
    ASSERT_TRUE(event);
    EXPECT_EQ(state.apply(*event), ModerationApplyResult::Applied);
    ASSERT_TRUE(state.pinnedMessageId());
    EXPECT_EQ(*state.pinnedMessageId(), "pin");
    const auto snapshot = moderationSnapshot(state);
    EXPECT_EQ(snapshot.scope, scope());
    EXPECT_EQ(snapshot.pinnedMessageId, state.pinnedMessageId());
    EXPECT_EQ(snapshot.deletedMessageIds, state.deletedMessageIds());

    StateOperation ignored =
        DeleteNonRantMessagesEvent{{"message-1"}, false};
    EXPECT_FALSE(moderationEventFromStateOperation(ignored, scope(), 2));
}

TEST(RumbleModerationCapabilities,
     RecomputesPerAccountAndChannelWithoutClaimingMutationSupport)
{
    ModerationState state(scope());
    EXPECT_EQ(state.capabilities().get(ModerationCapability::DeleteMessage),
              ModerationAvailability::Unauthorized);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::ObserveDeletes),
              ModerationAvailability::Available);

    EXPECT_EQ(state.apply({1,
                           ModerationRoleSnapshot{
                               scope(), {ModerationRole::Moderator}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::DeleteMessage),
              ModerationAvailability::Unsupported);

    EXPECT_EQ(state.apply({2,
                           ModerationRoleSnapshot{
                               scope(), {ModerationRole::Broadcaster}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::MuteUser),
              ModerationAvailability::Unsupported);

    EXPECT_EQ(state.apply({3,
                           ModerationRoleSnapshot{
                               scope(), {ModerationRole::Subscriber}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::DeleteMessage),
              ModerationAvailability::Unauthorized);

    EXPECT_EQ(state.apply({4,
                           ModerationRoleSnapshot{
                               scope(), {ModerationRole::Moderator}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::DeleteMessage),
              ModerationAvailability::Unsupported);

    EXPECT_EQ(state.apply({5,
                           ModerationRoleSnapshot{scope(), {}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.capabilities().get(ModerationCapability::DeleteMessage),
              ModerationAvailability::Unauthorized);

    for (const auto capability : {
             ModerationCapability::DeleteMessage,
             ModerationCapability::PinMessage,
             ModerationCapability::UnpinMessage,
             ModerationCapability::MuteUser,
             ModerationCapability::UnmuteUser,
             ModerationCapability::BanUser,
             ModerationCapability::UnbanUser,
         })
    {
        EXPECT_EQ(state.capabilities().get(capability),
                  ModerationAvailability::Unauthorized);
        EXPECT_EQ(requestModerationMutation(capability).error(),
                  ModerationMutationError::Unsupported);
    }
}

}  // namespace
}  // namespace chatterino
