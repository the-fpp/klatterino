// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UserCardIdentity.hpp"

#include <gtest/gtest.h>

#include <unordered_map>

namespace chatterino {
namespace {

MessagePtrMut message(MessagePlatform platform, QString id,
                      QString login = QStringLiteral("same-name"),
                      QString display = QStringLiteral("Same Name"))
{
    auto result = std::make_shared<Message>();
    result->platform = platform;
    result->userID = std::move(id);
    result->loginName = std::move(login);
    result->displayName = std::move(display);
    return result;
}

TEST(UserCardIdentity, ProviderAndStableIDArePartOfCacheKey)
{
    const UserCardIdentityKey twitch{UserCardProvider::Twitch,
                                     UserCardIdentityKind::StableProviderID,
                                     QStringLiteral("user-1")};
    const UserCardIdentityKey kick{UserCardProvider::Kick,
                                   UserCardIdentityKind::StableProviderID,
                                   QStringLiteral("user-1")};
    const UserCardIdentityKey rumble{UserCardProvider::Rumble,
                                     UserCardIdentityKind::StableProviderID,
                                     QStringLiteral("user-1")};

    EXPECT_NE(twitch, kick);
    EXPECT_NE(twitch, rumble);
    EXPECT_NE(kick, rumble);

    std::unordered_map<UserCardIdentityKey, QString, UserCardIdentityKeyHash>
        cache;
    cache.emplace(twitch, QStringLiteral("twitch"));
    cache.emplace(kick, QStringLiteral("kick"));
    cache.emplace(rumble, QStringLiteral("rumble"));
    EXPECT_EQ(cache.size(), 3U);
    EXPECT_EQ(cache.at(rumble), QStringLiteral("rumble"));
}

TEST(UserCardIdentity, ProviderScopedFallbackCannotAliasOtherPlatforms)
{
    const auto twitch = userCardIdentityFromName(QStringLiteral("Same-Name"),
                                                 MessagePlatform::AnyOrTwitch);
    const auto kick = userCardIdentityFromName(QStringLiteral("same-name"),
                                               MessagePlatform::Kick);
    const auto rumble = userCardIdentityFromName(QStringLiteral("SAME-NAME"),
                                                 MessagePlatform::Rumble);

    EXPECT_EQ(twitch.key.value, QStringLiteral("same-name"));
    EXPECT_EQ(kick.key.value, QStringLiteral("same-name"));
    EXPECT_EQ(rumble.key.value, QStringLiteral("same-name"));
    EXPECT_NE(twitch.key, kick.key);
    EXPECT_NE(twitch.key, rumble.key);
    EXPECT_NE(kick.key, rumble.key);
}

TEST(UserCardIdentity, ClickedRumbleAuthorCarriesAuthoritativeMessageIdentity)
{
    auto source =
        message(MessagePlatform::Rumble, QStringLiteral("rumble-42"),
                QStringLiteral("same-name"), QStringLiteral("Rumble Display"));
    source->usernameColor = QColor(QStringLiteral("#123456"));
    source->rumble = RumbleMessageMetadata{
        .channelID = QStringLiteral("channel-1"),
        .badgeIDs = {QStringLiteral("verified"),
                     QStringLiteral("recurring_subscription")},
        .roleIDs = {QStringLiteral("moderator")},
        .source = QStringLiteral("history"),
    };

    const auto identity =
        userCardIdentityFromMessage(*source, QStringLiteral("same-name"));
    EXPECT_EQ(identity.key.provider, UserCardProvider::Rumble);
    EXPECT_EQ(identity.key.kind, UserCardIdentityKind::StableProviderID);
    EXPECT_EQ(identity.key.value, QStringLiteral("rumble-42"));
    EXPECT_EQ(identity.loginName, QStringLiteral("same-name"));
    EXPECT_EQ(identity.displayName, QStringLiteral("Rumble Display"));
    EXPECT_EQ(identity.usernameColor, QColor(QStringLiteral("#123456")));
    EXPECT_EQ(identity.rumbleBadgeIDs,
              (QStringList{QStringLiteral("verified"),
                           QStringLiteral("recurring_subscription")}));
    EXPECT_EQ(identity.rumbleRoleIDs, QStringList{QStringLiteral("moderator")});
}

TEST(UserCardIdentity, NonAuthorLinkNeverBorrowsMessageAuthorIDOrMetadata)
{
    auto source = message(MessagePlatform::Rumble, QStringLiteral("author-id"),
                          QStringLiteral("author"));
    source->rumble = RumbleMessageMetadata{
        .badgeIDs = {QStringLiteral("moderator")},
    };

    const auto identity =
        userCardIdentityFromMessage(*source, QStringLiteral("mentioned-user"));
    EXPECT_EQ(identity.key.provider, UserCardProvider::Rumble);
    EXPECT_EQ(identity.key.kind, UserCardIdentityKind::ProviderScopedLogin);
    EXPECT_EQ(identity.key.value, QStringLiteral("mentioned-user"));
    EXPECT_TRUE(identity.rumbleBadgeIDs.isEmpty());
}

TEST(UserCardIdentity, RumbleHasNoProfileLookupOrForeignControls)
{
    const auto rumble = userCardIdentityFromName(QStringLiteral("same-name"),
                                                 MessagePlatform::Rumble);
    EXPECT_EQ(userCardProfileLookup(rumble), UserCardProfileLookup::None);
    EXPECT_EQ(userCardSurfacePolicy(rumble),
              (UserCardSurfacePolicy{
                  .showAvatar = false,
                  .showAccountMetadata = false,
                  .showTwitchProfileAction = false,
                  .showCommonProfileActions = false,
                  .showBlock = false,
                  .showIgnoreHighlights = false,
                  .showNotes = false,
                  .showModeratorActions = false,
              }));

    const auto twitch = userCardIdentityFromName(QStringLiteral("same-name"),
                                                 MessagePlatform::AnyOrTwitch);
    const auto kick = userCardIdentityFromName(QStringLiteral("same-name"),
                                               MessagePlatform::Kick);
    EXPECT_EQ(userCardProfileLookup(twitch), UserCardProfileLookup::Twitch);
    EXPECT_EQ(userCardProfileLookup(kick), UserCardProfileLookup::Kick);
    EXPECT_TRUE(userCardSurfacePolicy(twitch).showTwitchProfileAction);
    EXPECT_TRUE(userCardSurfacePolicy(kick).showAvatar);
}

TEST(UserCardIdentity, DelayedCrossProviderAndOldGenerationResultsAreStale)
{
    const auto twitch = userCardIdentityFromName(QStringLiteral("same-name"),
                                                 MessagePlatform::AnyOrTwitch);
    const auto rumble = userCardIdentityFromName(QStringLiteral("same-name"),
                                                 MessagePlatform::Rumble);

    const UserCardRequestToken delayedTwitch{twitch.key, 7};
    const UserCardRequestToken currentRumble{rumble.key, 8};
    EXPECT_FALSE(userCardRequestIsCurrent(delayedTwitch, rumble.key, 8));
    EXPECT_FALSE(userCardRequestIsCurrent(currentRumble, rumble.key, 9));
    EXPECT_TRUE(userCardRequestIsCurrent(currentRumble, rumble.key, 8));
}

TEST(UserCardIdentity, StableIdentityFiltersSameNamedMixedPlatformMessages)
{
    auto clicked = message(MessagePlatform::Rumble, QStringLiteral("rumble-1"));
    const auto identity =
        userCardIdentityFromMessage(*clicked, QStringLiteral("same-name"));

    auto twitch =
        message(MessagePlatform::AnyOrTwitch, QStringLiteral("twitch-1"));
    auto kick = message(MessagePlatform::Kick, QStringLiteral("kick-1"));
    auto otherRumble =
        message(MessagePlatform::Rumble, QStringLiteral("rumble-2"));
    auto missingID = message(MessagePlatform::Rumble, {});
    auto history = message(MessagePlatform::Rumble, QStringLiteral("rumble-1"));
    auto realtime =
        message(MessagePlatform::Rumble, QStringLiteral("rumble-1"));
    auto localEcho =
        message(MessagePlatform::Rumble, QStringLiteral("rumble-1"));
    auto reconciled =
        message(MessagePlatform::Rumble, QStringLiteral("rumble-1"));

    EXPECT_FALSE(messageMatchesUserCard(identity, *twitch));
    EXPECT_FALSE(messageMatchesUserCard(identity, *kick));
    EXPECT_FALSE(messageMatchesUserCard(identity, *otherRumble));
    EXPECT_FALSE(messageMatchesUserCard(identity, *missingID));
    EXPECT_TRUE(messageMatchesUserCard(identity, *history));
    EXPECT_TRUE(messageMatchesUserCard(identity, *realtime));
    EXPECT_TRUE(messageMatchesUserCard(identity, *localEcho));
    EXPECT_TRUE(messageMatchesUserCard(identity, *reconciled));
}

TEST(UserCardIdentity, RumbleFallbackFilteringRemainsProviderScoped)
{
    const auto identity = userCardIdentityFromName(QStringLiteral("same-name"),
                                                   MessagePlatform::Rumble);
    const auto rumble =
        message(MessagePlatform::Rumble, {}, QStringLiteral("SAME-NAME"));
    const auto twitch =
        message(MessagePlatform::AnyOrTwitch, {}, QStringLiteral("same-name"));
    const auto kick =
        message(MessagePlatform::Kick, {}, QStringLiteral("same-name"));

    EXPECT_TRUE(messageMatchesUserCard(identity, *rumble));
    EXPECT_FALSE(messageMatchesUserCard(identity, *twitch));
    EXPECT_FALSE(messageMatchesUserCard(identity, *kick));
}

TEST(UserCardIdentity, TwitchAndKickFilteringRetainProviderBehavior)
{
    auto twitchSource =
        message(MessagePlatform::AnyOrTwitch, QStringLiteral("twitch-1"));
    const auto twitchIdentity =
        userCardIdentityFromMessage(*twitchSource, QStringLiteral("same-name"));
    auto subscription = message(MessagePlatform::AnyOrTwitch, {}, {});
    subscription->flags.set(MessageFlag::Subscription);
    subscription->messageText = QStringLiteral("same-name subscribed");
    EXPECT_TRUE(messageMatchesUserCard(twitchIdentity, *subscription));

    auto kickSource = message(MessagePlatform::Kick, QStringLiteral("kick-1"));
    const auto kickIdentity =
        userCardIdentityFromMessage(*kickSource, QStringLiteral("same-name"));
    const auto sameKick =
        message(MessagePlatform::Kick, QStringLiteral("kick-1"));
    const auto otherKick =
        message(MessagePlatform::Kick, QStringLiteral("kick-2"));
    EXPECT_TRUE(messageMatchesUserCard(kickIdentity, *sameKick));
    EXPECT_FALSE(messageMatchesUserCard(kickIdentity, *otherKick));

    auto whisper =
        message(MessagePlatform::AnyOrTwitch, QStringLiteral("twitch-1"));
    whisper->flags.set(MessageFlag::Whisper);
    EXPECT_FALSE(messageMatchesUserCard(twitchIdentity, *whisper));
}

TEST(UserCardIdentity, RumbleMetadataLabelsAreSafeAndDeterministic)
{
    EXPECT_EQ(rumbleUserCardMetadataLabels(
                  {QStringLiteral("moderator"),
                   QStringLiteral("recurring_subscription"),
                   QStringLiteral("moderator"), QStringLiteral("whale-blue"),
                   QStringLiteral("<not metadata>")}),
              (QStringList{QStringLiteral("Moderator"),
                           QStringLiteral("Recurring Subscription"),
                           QStringLiteral("Whale Blue")}));
}

}  // namespace
}  // namespace chatterino
