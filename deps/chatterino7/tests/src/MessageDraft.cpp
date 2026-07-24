// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageDraft.hpp"
#include "messages/MessageDraftCapability.hpp"
#include "messages/MessageDraftTracker.hpp"

#include <gtest/gtest.h>

using namespace chatterino;

namespace {

DraftEmoteCandidate candidate(QString provider, QString id, QString text,
                              QString platform = {},
                              std::optional<QString> channel = std::nullopt,
                              std::optional<QString> account = std::nullopt)
{
    return {
        .identity = {.provider = std::move(provider),
                     .id = EmoteId{std::move(id)}},
        .insertionText = std::move(text),
        .availability =
            {
                .platform = std::move(platform),
                .channelID = std::move(channel),
                .accountID = std::move(account),
            },
    };
}

MessageSendContext context(QString platform = QStringLiteral("twitch"),
                           QString channel = QStringLiteral("channel-a"),
                           QString account = QStringLiteral("account-a"))
{
    return {
        .platform = std::move(platform),
        .channelID = std::move(channel),
        .accountID = std::move(account),
        .writable = true,
        .authenticated = true,
    };
}

}  // namespace

TEST(MessageDraft, PlainTextIsCompatibleWithWritableChannels)
{
    const auto draft =
        MessageDraft::fromPlainText(QStringLiteral("Kappa hello"));
    EXPECT_TRUE(evaluateMessageDraft(draft, context(QStringLiteral("twitch")))
                    .sendable);
    EXPECT_TRUE(
        evaluateMessageDraft(draft, context(QStringLiteral("kick"))).sendable);
    EXPECT_TRUE(evaluateMessageDraft(draft, context(QStringLiteral("rumble")))
                    .sendable);
}

TEST(MessageDraft, ReportsWritableAuthenticationLengthAndProviderConstraints)
{
    const auto draft = MessageDraft::fromPlainText(QStringLiteral("hello"));
    auto ctx = context();
    ctx.writable = false;
    ctx.authenticated = false;
    ctx.maxMessageLength = 4;
    ctx.providerConstraints.append(QStringLiteral("Followers-only mode"));

    const auto result = evaluateMessageDraft(draft, ctx);
    ASSERT_EQ(result.rejections.size(), 4);
    EXPECT_EQ(result.rejections[0].code,
              MessageDraftRejectionCode::NotWritable);
    EXPECT_EQ(result.rejections[1].code,
              MessageDraftRejectionCode::NotAuthenticated);
    EXPECT_EQ(result.rejections[2].code, MessageDraftRejectionCode::TooLong);
    EXPECT_EQ(result.rejections[3].code,
              MessageDraftRejectionCode::ProviderConstraint);
}

TEST(MessageDraft, EvaluatesGlobalChannelAndAccountAvailability)
{
    const std::vector candidates{
        candidate(QStringLiteral("emoji"), QStringLiteral("wave"),
                  QStringLiteral(":wave:")),
        candidate(QStringLiteral("7tv"), QStringLiteral("channel"),
                  QStringLiteral("ChannelOnly"), QStringLiteral("twitch"),
                  QStringLiteral("channel-a")),
        candidate(QStringLiteral("twitch"), QStringLiteral("account"),
                  QStringLiteral("AccountOnly"), QStringLiteral("twitch"),
                  std::nullopt, QStringLiteral("account-a")),
    };
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral(":wave: ChannelOnly AccountOnly"), candidates);
    ASSERT_EQ(draft.emotes.size(), 3);

    EXPECT_TRUE(evaluateMessageDraft(draft, context()).sendable);

    const auto otherChannel = evaluateMessageDraft(
        draft, context(QStringLiteral("twitch"), QStringLiteral("channel-b")));
    EXPECT_FALSE(otherChannel.sendable);
    EXPECT_EQ(otherChannel.rejections.size(), 1);
    EXPECT_EQ(otherChannel.rejections[0].code,
              MessageDraftRejectionCode::UnsupportedEmote);

    const auto otherAccount = evaluateMessageDraft(
        draft, context(QStringLiteral("twitch"), QStringLiteral("channel-a"),
                       QStringLiteral("account-b")));
    EXPECT_FALSE(otherAccount.sendable);
    EXPECT_EQ(otherAccount.rejections.size(), 1);

    const auto otherPlatform = evaluateMessageDraft(
        draft, context(QStringLiteral("kick"), QStringLiteral("channel-a")));
    EXPECT_FALSE(otherPlatform.sendable);
    EXPECT_EQ(otherPlatform.rejections.size(), 2);
}

TEST(MessageDraft, DuplicateDisplayNameDoesNotLoseProviderIdentity)
{
    const std::vector twitchOnly{
        candidate(QStringLiteral("twitch"), QStringLiteral("25"),
                  QStringLiteral("SameName"), QStringLiteral("twitch")),
    };
    const std::vector kickOnly{
        candidate(QStringLiteral("kick"), QStringLiteral("9001"),
                  QStringLiteral("SameName"), QStringLiteral("kick")),
    };

    const auto twitchDraft =
        MessageDraft::reconstruct(QStringLiteral("SameName"), twitchOnly);
    const auto kickDraft =
        MessageDraft::reconstruct(QStringLiteral("SameName"), kickOnly);
    ASSERT_EQ(twitchDraft.emotes.size(), 1);
    ASSERT_EQ(kickDraft.emotes.size(), 1);
    EXPECT_NE(twitchDraft.emotes[0].identity, kickDraft.emotes[0].identity);
    EXPECT_TRUE(
        evaluateMessageDraft(twitchDraft, context(QStringLiteral("twitch")))
            .sendable);
    EXPECT_FALSE(
        evaluateMessageDraft(twitchDraft, context(QStringLiteral("kick")))
            .sendable);
    EXPECT_TRUE(evaluateMessageDraft(kickDraft, context(QStringLiteral("kick")))
                    .sendable);
}

TEST(MessageDraft, SameTextProviderIdentitiesRemainExactAlternatives)
{
    const std::vector candidates{
        candidate(QStringLiteral("twitch"), QStringLiteral("25"),
                  QStringLiteral("SameName"), QStringLiteral("twitch")),
        candidate(QStringLiteral("kick"), QStringLiteral("9001"),
                  QStringLiteral("SameName"), QStringLiteral("kick")),
    };
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("SameName NotAnEmote"), candidates);

    ASSERT_EQ(draft.emotes.size(), 1U);
    ASSERT_EQ(draft.emotes[0].identityAlternatives.size(), 1U);
    EXPECT_EQ(draft.emotes[0].identity.provider, QStringLiteral("twitch"));
    EXPECT_EQ(draft.emotes[0].identityAlternatives[0].identity.provider,
              QStringLiteral("kick"));
    EXPECT_TRUE(evaluateMessageDraft(draft, context(QStringLiteral("twitch")))
                    .sendable);
    EXPECT_TRUE(
        evaluateMessageDraft(draft, context(QStringLiteral("kick"))).sendable);

    auto twitch = context(QStringLiteral("twitch"));
    twitch.emoteCapabilitiesComplete = true;
    twitch.emoteCapabilities.push_back({
        .identity = candidates[0].identity,
        .insertionText = candidates[0].insertionText,
        .availability = candidates[0].availability,
    });
    EXPECT_TRUE(evaluateMessageDraft(draft, twitch).sendable);

    auto unsupported = context(QStringLiteral("rumble"));
    unsupported.emoteCapabilitiesComplete = true;
    EXPECT_FALSE(evaluateMessageDraft(draft, unsupported).sendable);
}

TEST(MessageDraft, InferredUnsupportedEmotesRemainEligibleAsPlainText)
{
    const auto emote =
        candidate(QStringLiteral("7tv"), QStringLiteral("typed"),
                  QStringLiteral("TypedOnly"), QStringLiteral("twitch"),
                  QStringLiteral("channel-a"));
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("TypedOnly TypedOnly"), std::span{&emote, 1});
    ASSERT_EQ(draft.emotes.size(), 2U);
    EXPECT_TRUE(draft.emotes[0].inferred);
    EXPECT_TRUE(draft.emotes[1].inferred);

    auto supported = context();
    supported.emoteCapabilitiesComplete = true;
    supported.emoteCapabilities.push_back({
        .identity = emote.identity,
        .insertionText = emote.insertionText,
        .availability = emote.availability,
    });
    const auto supportedEvaluation = evaluateMessageDraft(draft, supported);
    EXPECT_TRUE(supportedEvaluation.sendable);
    EXPECT_TRUE(supportedEvaluation.sendableWithInferredEmoteFallback);
    EXPECT_EQ(supportedEvaluation.supportedEmoteOccurrences, 2U);
    EXPECT_EQ(supportedEvaluation.unsupportedInferredEmoteOccurrences, 0U);

    auto unsupported = context();
    unsupported.emoteCapabilitiesComplete = true;
    const auto unsupportedEvaluation = evaluateMessageDraft(draft, unsupported);
    EXPECT_FALSE(unsupportedEvaluation.sendable);
    EXPECT_TRUE(unsupportedEvaluation.sendableWithInferredEmoteFallback);
    EXPECT_EQ(unsupportedEvaluation.supportedEmoteOccurrences, 0U);
    EXPECT_EQ(unsupportedEvaluation.unsupportedInferredEmoteOccurrences, 2U);
}

TEST(MessageDraft, InferredFallbackDoesNotBypassHardConstraints)
{
    const auto emote =
        candidate(QStringLiteral("7tv"), QStringLiteral("typed"),
                  QStringLiteral("TypedOnly"), QStringLiteral("twitch"));
    auto draft = MessageDraft::reconstruct(QStringLiteral("TypedOnly"),
                                           std::span{&emote, 1});
    draft.provenanceValid = false;

    auto blocked = context();
    blocked.writable = false;
    blocked.authenticated = false;
    blocked.maxMessageLength = 4;
    blocked.providerConstraints.append(QStringLiteral("Provider blocked"));
    blocked.emoteCapabilitiesComplete = true;

    const auto evaluation = evaluateMessageDraft(draft, blocked);
    EXPECT_FALSE(evaluation.sendable);
    EXPECT_FALSE(evaluation.sendableWithInferredEmoteFallback);
    EXPECT_EQ(evaluation.unsupportedInferredEmoteOccurrences, 1U);
    EXPECT_EQ(evaluation.rejections.size(), 6U);
    const auto hasCode = [&](MessageDraftRejectionCode code) {
        return std::ranges::any_of(evaluation.rejections,
                                   [code](const auto &rejection) {
                                       return rejection.code == code;
                                   });
    };
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::NotWritable));
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::NotAuthenticated));
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::TooLong));
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::InvalidProvenance));
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::UnsupportedEmote));
    EXPECT_TRUE(hasCode(MessageDraftRejectionCode::ProviderConstraint));
}

TEST(MessageDraft, ExplicitUnsupportedEmoteRemainsIneligible)
{
    const auto emote =
        candidate(QStringLiteral("7tv"), QStringLiteral("selected"),
                  QStringLiteral("Selected"), QStringLiteral("twitch"),
                  QStringLiteral("channel-a"));
    MessageDraft draft{
        .text = emote.insertionText,
        .emotes = {{
            .identity = emote.identity,
            .insertionText = emote.insertionText,
            .start = 0,
            .length = emote.insertionText.size(),
            .availability = emote.availability,
        }},
    };
    ASSERT_FALSE(draft.emotes[0].inferred);

    auto unsupported = context();
    unsupported.emoteCapabilitiesComplete = true;
    const auto evaluation = evaluateMessageDraft(draft, unsupported);
    EXPECT_FALSE(evaluation.sendable);
    EXPECT_FALSE(evaluation.sendableWithInferredEmoteFallback);
    EXPECT_EQ(evaluation.supportedEmoteOccurrences, 0U);
    EXPECT_EQ(evaluation.unsupportedInferredEmoteOccurrences, 0U);
}

TEST(MessageDraft, FullSevenTvEmoteUrlIsNeverInferredAsEmote)
{
    const auto url =
        QStringLiteral("https://7tv.app/emotes/01H0123456789ABCDEFGHJKMNP");
    const auto misleadingCapability =
        candidate(QStringLiteral("7tv"), QStringLiteral("url-collision"), url,
                  QStringLiteral("twitch"));

    const auto draft =
        MessageDraft::reconstruct(url, std::span{&misleadingCapability, 1});
    EXPECT_EQ(draft.text, url);
    EXPECT_TRUE(draft.emotes.empty());
}

TEST(MessageDraft, ExplicitSelectionOverridesTypedIdentityUnionAtItsRange)
{
    const std::vector candidates{
        candidate(QStringLiteral("twitch"), QStringLiteral("25"),
                  QStringLiteral("SameName"), QStringLiteral("twitch")),
        candidate(QStringLiteral("kick"), QStringLiteral("9001"),
                  QStringLiteral("SameName"), QStringLiteral("kick")),
    };
    MessageDraft selected{
        .text = QStringLiteral("SameName"),
        .emotes = {{
            .identity = candidates[0].identity,
            .insertionText = candidates[0].insertionText,
            .start = 0,
            .length = candidates[0].insertionText.size(),
            .availability = candidates[0].availability,
        }},
    };

    const auto reconstructed =
        MessageDraft::reconstructUntracked(std::move(selected), candidates);
    ASSERT_EQ(reconstructed.emotes.size(), 1U);
    EXPECT_TRUE(reconstructed.emotes[0].identityAlternatives.empty());
    EXPECT_TRUE(evaluateMessageDraft(
                    reconstructed, context(QStringLiteral("twitch")))
                    .sendable);
    EXPECT_FALSE(evaluateMessageDraft(
                     reconstructed, context(QStringLiteral("kick")))
                     .sendable);
}

TEST(MessageDraft, RejectsMalformedProvenance)
{
    auto draft = MessageDraft::fromPlainText(QStringLiteral("Kappa"));
    draft.emotes.push_back({
        .identity = {.provider = QStringLiteral("twitch"),
                     .id = EmoteId{QStringLiteral("25")}},
        .insertionText = QStringLiteral("NotKappa"),
        .start = 0,
        .length = 5,
        .availability = {.platform = QStringLiteral("twitch")},
    });

    const auto result = evaluateMessageDraft(draft, context());
    ASSERT_EQ(result.rejections.size(), 1);
    EXPECT_EQ(result.rejections[0].code,
              MessageDraftRejectionCode::MalformedEmote);
}

TEST(MessageDraft, CompleteCapabilitySnapshotRejectsRemovedEmote)
{
    const auto selected = candidate(
        QStringLiteral("7tv"), QStringLiteral("stable-id"),
        QStringLiteral("PresentThenRemoved"), QStringLiteral("twitch"),
        QStringLiteral("channel-a"));
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("PresentThenRemoved"), std::span{&selected, 1});

    auto present = context();
    present.emoteCapabilitiesComplete = true;
    present.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });
    EXPECT_TRUE(evaluateMessageDraft(draft, present).sendable);

    auto removed = present;
    removed.emoteCapabilities.clear();
    const auto result = evaluateMessageDraft(draft, removed);
    ASSERT_FALSE(result.sendable);
    ASSERT_EQ(result.rejections.size(), 1U);
    EXPECT_EQ(result.rejections[0].code,
              MessageDraftRejectionCode::UnsupportedEmote);
}

TEST(MessageDraft, UnknownScopeFailsConservatively)
{
    auto unresolved = candidate(
        QStringLiteral("7tv"), QStringLiteral("stable-id"),
        QStringLiteral("UnknownScope"));
    unresolved.availability.known = false;
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("UnknownScope"), std::span{&unresolved, 1});

    const auto result = evaluateMessageDraft(draft, context());
    ASSERT_FALSE(result.sendable);
    ASSERT_EQ(result.rejections.size(), 1U);
    EXPECT_EQ(result.rejections[0].code,
              MessageDraftRejectionCode::UnsupportedEmote);
}

TEST(MessageDraft, CompleteCapabilitySnapshotRejectsRenamedEmote)
{
    const auto selected = candidate(
        QStringLiteral("7tv"), QStringLiteral("stable-id"),
        QStringLiteral("OldName"), QStringLiteral("twitch"),
        QStringLiteral("channel-a"));
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("OldName"), std::span{&selected, 1});

    auto renamed = context();
    renamed.emoteCapabilitiesComplete = true;
    renamed.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = QStringLiteral("NewName"),
        .availability = selected.availability,
    });
    const auto rejected = evaluateMessageDraft(draft, renamed);
    ASSERT_FALSE(rejected.sendable);
    ASSERT_EQ(rejected.rejections.size(), 1U);
    EXPECT_EQ(rejected.rejections[0].code,
              MessageDraftRejectionCode::UnsupportedEmote);

    renamed.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });
    EXPECT_TRUE(evaluateMessageDraft(draft, renamed).sendable);
}

TEST(MessageDraft, EmojiShortcodeMustStillBeCurrent)
{
    const auto selected = candidate(
        QStringLiteral("emoji"), QStringLiteral("1f600"),
        QStringLiteral(":old_smile:"));
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral(":old_smile:"), std::span{&selected, 1});

    auto current = context(QStringLiteral("kick"));
    current.emoteCapabilitiesComplete = true;
    current.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = QStringLiteral(":new_smile:"),
        .availability = {},
    });
    EXPECT_FALSE(evaluateMessageDraft(draft, current).sendable);

    current.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = {},
    });
    EXPECT_TRUE(evaluateMessageDraft(draft, current).sendable);
}

TEST(MessageDraft, PlatformGlobalDoesNotWidenToEmojiGlobal)
{
    const auto thirdParty = candidate(
        QStringLiteral("7tv"), QStringLiteral("global-id"),
        QStringLiteral("PlatformGlobal"), QStringLiteral("twitch"));
    const auto emoji = candidate(QStringLiteral("emoji"),
                                 QStringLiteral("1f44b"),
                                 QStringLiteral(":wave:"));

    const auto thirdPartyDraft = MessageDraft::reconstruct(
        QStringLiteral("PlatformGlobal"), std::span{&thirdParty, 1});
    const auto emojiDraft = MessageDraft::reconstruct(
        QStringLiteral(":wave:"), std::span{&emoji, 1});

    auto kick = context(QStringLiteral("kick"));
    kick.emoteCapabilitiesComplete = true;
    kick.emoteCapabilities.push_back({
        .identity = thirdParty.identity,
        .insertionText = thirdParty.insertionText,
        .availability = {.platform = QStringLiteral("kick")},
    });
    kick.emoteCapabilities.push_back({
        .identity = emoji.identity,
        .insertionText = emoji.insertionText,
        .availability = {},
    });

    EXPECT_FALSE(evaluateMessageDraft(thirdPartyDraft, kick).sendable);
    EXPECT_TRUE(evaluateMessageDraft(emojiDraft, kick).sendable);
}

TEST(MessageDraftCapability, PreservesMapAliasesAndEveryEmojiShortcode)
{
    const auto emote = std::shared_ptr<Emote>(new Emote{
        .name = EmoteName{QStringLiteral("Canonical")},
        .id = EmoteId{QStringLiteral("stable-id")},
    });
    EmoteMap emotes;
    emotes.emplace(EmoteName{QStringLiteral("Alias")}, emote);

    std::vector<DraftEmoteCapability> capabilities;
    appendDraftEmoteCapabilities(
        capabilities, emotes, QStringLiteral("7tv"),
        {.platform = QStringLiteral("twitch")});
    ASSERT_EQ(capabilities.size(), 1U);
    EXPECT_EQ(capabilities[0].insertionText, QStringLiteral("Alias"));
    EXPECT_EQ(capabilities[0].availability.platform,
              QStringLiteral("twitch"));

    auto emoji = std::make_shared<EmojiData>();
    emoji->unifiedCode = QStringLiteral("1f600");
    emoji->shortCodes = {QStringLiteral("grinning"),
                         QStringLiteral("smile")};
    appendDraftEmojiCapabilities(capabilities, {emoji});

    ASSERT_EQ(capabilities.size(), 3U);
    EXPECT_EQ(capabilities[1].insertionText,
              QStringLiteral(":grinning:"));
    EXPECT_EQ(capabilities[2].insertionText, QStringLiteral(":smile:"));
    EXPECT_TRUE(capabilities[1].availability.platform.isEmpty());
    EXPECT_TRUE(capabilities[2].availability.platform.isEmpty());
}

TEST(MessageDraftTracker, TracksRepeatedSelectionsByExactRange)
{
    MessageDraftTracker tracker;
    const auto selected = candidate(
        QStringLiteral("twitch"), QStringLiteral("25"),
        QStringLiteral("Same"), QStringLiteral("twitch"));
    const QString text = QStringLiteral("Same Same");

    EXPECT_TRUE(tracker.recordSelection(0, selected, text));
    EXPECT_TRUE(tracker.recordSelection(5, selected, text));
    const auto draft = tracker.snapshot(text);
    ASSERT_EQ(draft.emotes.size(), 2U);
    EXPECT_EQ(draft.emotes[0].start, 0);
    EXPECT_EQ(draft.emotes[1].start, 5);
}

TEST(MessageDraftTracker, EditsBesideTokensShiftWithoutTransferringIdentity)
{
    MessageDraftTracker tracker;
    const auto selected = candidate(
        QStringLiteral("twitch"), QStringLiteral("25"),
        QStringLiteral("Kappa"), QStringLiteral("twitch"));
    EXPECT_TRUE(
        tracker.recordSelection(6, selected, QStringLiteral("hello Kappa")));

    tracker.contentsChanged(0, 0, 2, QStringLiteral("++hello Kappa"));
    auto draft = tracker.snapshot(QStringLiteral("++hello Kappa"));
    ASSERT_EQ(draft.emotes.size(), 1U);
    EXPECT_EQ(draft.emotes[0].start, 8);

    tracker.contentsChanged(13, 0, 1, QStringLiteral("++hello Kappa!"));
    draft = tracker.snapshot(QStringLiteral("++hello Kappa!"));
    ASSERT_EQ(draft.emotes.size(), 1U);
    EXPECT_EQ(draft.emotes[0].start, 8);
    EXPECT_EQ(draft.emotes[0].length, 5);
}

TEST(MessageDraftTracker, IdenticalReplacementDropsStaleIdentity)
{
    MessageDraftTracker tracker;
    const auto selected = candidate(
        QStringLiteral("twitch"), QStringLiteral("25"),
        QStringLiteral("Kappa"), QStringLiteral("twitch"));
    EXPECT_TRUE(tracker.recordSelection(0, selected, QStringLiteral("Kappa")));

    tracker.contentsChanged(0, 5, 5, QStringLiteral("Kappa"));
    const auto draft = tracker.snapshot(QStringLiteral("Kappa"));
    EXPECT_TRUE(draft.emotes.empty());
    EXPECT_TRUE(draft.provenanceValid);
}

TEST(MessageDraftTracker, OverlapAndOverflowTrimCannotRetainWrongRange)
{
    MessageDraftTracker tracker;
    const auto first = candidate(
        QStringLiteral("twitch"), QStringLiteral("25"),
        QStringLiteral("Kappa"), QStringLiteral("twitch"));
    const auto second = candidate(
        QStringLiteral("7tv"), QStringLiteral("abc"),
        QStringLiteral("Seven"), QStringLiteral("twitch"));
    EXPECT_TRUE(tracker.recordSelection(
        0, first, QStringLiteral("Kappa middle Seven")));
    EXPECT_TRUE(tracker.recordSelection(
        13, second, QStringLiteral("Kappa middle Seven")));

    tracker.contentsChanged(0, 6, 0, QStringLiteral("middle Seven"));
    auto draft = tracker.snapshot(QStringLiteral("middle Seven"));
    ASSERT_EQ(draft.emotes.size(), 1U);
    EXPECT_EQ(draft.emotes[0].identity, second.identity);
    EXPECT_EQ(draft.emotes[0].start, 7);

    tracker.contentsChanged(7, 5, 0, QStringLiteral("middle "));
    draft = tracker.snapshot(QStringLiteral("middle "));
    EXPECT_TRUE(draft.emotes.empty());
}

TEST(MessageDraftTracker, FailedTypedInsertionRejectsUntilNextEditOrClear)
{
    MessageDraftTracker tracker;
    const auto selected = candidate(
        QStringLiteral("twitch"), QStringLiteral("25"),
        QStringLiteral("Kappa"), QStringLiteral("twitch"));
    EXPECT_FALSE(
        tracker.recordSelection(1, selected, QStringLiteral("Kappa")));
    auto draft = tracker.snapshot(QStringLiteral("Kappa"));
    EXPECT_FALSE(draft.provenanceValid);
    const auto rejected = evaluateMessageDraft(draft, context());
    ASSERT_FALSE(rejected.sendable);
    ASSERT_FALSE(rejected.rejections.empty());
    EXPECT_EQ(rejected.rejections[0].code,
              MessageDraftRejectionCode::InvalidProvenance);

    tracker.contentsChanged(5, 0, 1, QStringLiteral("Kappa "));
    draft = tracker.snapshot(QStringLiteral("Kappa "));
    EXPECT_FALSE(draft.provenanceValid);

    tracker.contentsChanged(0, 1, 1, QStringLiteral("kappa "));
    draft = tracker.snapshot(QStringLiteral("kappa "));
    EXPECT_TRUE(draft.provenanceValid);

    EXPECT_FALSE(
        tracker.recordSelection(2, selected, QStringLiteral("kappa ")));
    tracker.clear();
    EXPECT_TRUE(tracker.snapshot({}).provenanceValid);
}

TEST(MessageDraftTracker, AmbiguousSelectionPersistsUntilTokenIsEdited)
{
    MessageDraftTracker tracker;
    tracker.recordUnresolvedSelection(0, 5, QStringLiteral("Kappa"));
    EXPECT_FALSE(tracker.snapshot(QStringLiteral("Kappa")).provenanceValid);

    tracker.contentsChanged(5, 0, 1, QStringLiteral("Kappa!"));
    EXPECT_FALSE(tracker.snapshot(QStringLiteral("Kappa!")).provenanceValid);

    tracker.contentsChanged(0, 1, 1, QStringLiteral("kappa!"));
    EXPECT_TRUE(tracker.snapshot(QStringLiteral("kappa!")).provenanceValid);
}
