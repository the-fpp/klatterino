// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace chatterino {

/// Stable provider identity for an emote. Display names are deliberately not
/// part of the identity because different providers can expose the same name.
struct DraftEmoteIdentity {
    QString provider;
    EmoteId id;

    bool operator==(const DraftEmoteIdentity &) const = default;
};

/// Availability attached to a completion candidate or reconstructed emote.
/// Empty fields widen availability: an empty platform is cross-platform, while
/// absent channel/account IDs mean the emote is not scoped to that value.
struct DraftEmoteAvailability {
    QString platform;
    std::optional<QString> channelID;
    std::optional<QString> accountID;
    /// False means that the producer could not establish whether the emote is
    /// global, account-scoped, or channel-scoped. Unknown scope is never
    /// guessed during routing.
    bool known = true;

    bool operator==(const DraftEmoteAvailability &) const = default;
};

/// One provider identity which can render a token. Plain text and tab
/// completion can legitimately resolve to more than one provider identity
/// when providers expose the same insertion text. The alternatives remain
/// exact: identity and availability are never widened independently.
struct DraftEmoteVariant {
    DraftEmoteIdentity identity;
    DraftEmoteAvailability availability;
    std::vector<DraftEmoteAvailability> availabilityAlternatives;

    bool operator==(const DraftEmoteVariant &) const = default;
};

struct DraftEmoteCandidate {
    DraftEmoteIdentity identity;
    QString insertionText;
    DraftEmoteAvailability availability;
    /// Aggregate completion can expose the same stable identity through more
    /// than one child. These are exact alternatives, not a widened scope.
    std::vector<DraftEmoteAvailability> availabilityAlternatives;
    /// Other provider identities which render the same insertion text. This is
    /// used only when the UI did not choose a provider-specific row (typed text
    /// or a deduplicated tab-completion row).
    std::vector<DraftEmoteVariant> identityAlternatives;

    bool operator==(const DraftEmoteCandidate &) const = default;
};

struct DraftEmoteOccurrence {
    DraftEmoteIdentity identity;
    QString insertionText;
    qsizetype start = 0;
    qsizetype length = 0;
    /// True when routing inferred this occurrence from ordinary typed text.
    /// Completion and picker selections remain explicit and must be supported
    /// by the selected destination.
    bool inferred = false;
    DraftEmoteAvailability availability;
    std::vector<DraftEmoteAvailability> availabilityAlternatives;
    std::vector<DraftEmoteVariant> identityAlternatives;
};

inline void appendDraftEmoteAvailability(
    std::vector<DraftEmoteAvailability> &availabilities,
    const DraftEmoteAvailability &availability)
{
    if (std::ranges::find(availabilities, availability) ==
        availabilities.end())
    {
        availabilities.push_back(availability);
    }
}

inline void mergeDraftEmoteVariant(DraftEmoteCandidate &destination,
                                   const DraftEmoteVariant &variant)
{
    auto mergeAvailabilities = [](auto &target, const auto &source,
                                  const auto &fallback) {
        if (target.empty())
        {
            target.push_back(fallback);
        }
        if (source.empty())
        {
            appendDraftEmoteAvailability(target, fallback);
            return;
        }
        for (const auto &availability : source)
        {
            appendDraftEmoteAvailability(target, availability);
        }
    };

    if (destination.identity == variant.identity)
    {
        mergeAvailabilities(destination.availabilityAlternatives,
                            variant.availabilityAlternatives,
                            variant.availability);
        return;
    }

    const auto existing = std::ranges::find(
        destination.identityAlternatives, variant.identity,
        &DraftEmoteVariant::identity);
    if (existing == destination.identityAlternatives.end())
    {
        destination.identityAlternatives.push_back(variant);
        return;
    }
    mergeAvailabilities(existing->availabilityAlternatives,
                        variant.availabilityAlternatives,
                        variant.availability);
}

/// Merge candidates which produce exactly the same text. Provider identities
/// remain paired with their own availability instead of becoming ambiguous or
/// widening one another.
inline bool mergeDraftEmoteCandidate(DraftEmoteCandidate &destination,
                                     const DraftEmoteCandidate &source)
{
    if (destination.insertionText != source.insertionText)
    {
        return false;
    }

    mergeDraftEmoteVariant(
        destination,
        {.identity = source.identity,
         .availability = source.availability,
         .availabilityAlternatives = source.availabilityAlternatives});
    for (const auto &alternative : source.identityAlternatives)
    {
        mergeDraftEmoteVariant(destination, alternative);
    }
    return true;
}

/// One exact emote capability captured from current provider state. Channels
/// that publish a complete capability snapshot let submission detect emotes
/// removed after completion without calling provider APIs during evaluation.
struct DraftEmoteCapability {
    DraftEmoteIdentity identity;
    /// Exact text that the provider currently recognizes for this identity.
    /// Stable IDs alone are insufficient because providers can rename emotes or
    /// expose the same identity under different aliases.
    QString insertionText;
    DraftEmoteAvailability availability;
};

/// A message draft plus the emotes whose provenance is known. Text that merely
/// resembles an emote remains plain text unless it reconstructs to exactly one
/// stable candidate identity.
struct MessageDraft {
    QString text;
    std::vector<DraftEmoteOccurrence> emotes;
    bool provenanceValid = true;
    /// When set by one SplitInput, multi-channel routing and completion are
    /// constrained to this provider platform without changing the aggregate's
    /// active child. Empty means automatic routing.
    std::optional<QString> destinationPlatformOverride;

    static MessageDraft fromPlainText(QString text)
    {
        return {.text = std::move(text)};
    }

    /// Reconstructs provenance in O(text length + candidate count). Different
    /// provider identities with the same insertion text are retained as exact
    /// alternatives; a destination is compatible when it supports any one of
    /// those identities.
    static MessageDraft reconstruct(
        QString text, std::span<const DraftEmoteCandidate> candidates)
    {
        struct Resolution {
            std::optional<DraftEmoteCandidate> candidate;
        };

        QHash<QString, Resolution> resolutions;
        resolutions.reserve(static_cast<qsizetype>(candidates.size()));
        for (const auto &candidate : candidates)
        {
            if (candidate.insertionText.isEmpty() ||
                candidate.identity.provider.isEmpty() ||
                candidate.identity.id.string.isEmpty())
            {
                continue;
            }

            auto &resolution = resolutions[candidate.insertionText];
            if (!resolution.candidate)
            {
                resolution.candidate = candidate;
                continue;
            }
            mergeDraftEmoteCandidate(*resolution.candidate, candidate);
        }

        MessageDraft draft{.text = std::move(text)};
        qsizetype start = 0;
        while (start < draft.text.size())
        {
            while (start < draft.text.size() && draft.text[start].isSpace())
            {
                ++start;
            }
            if (start == draft.text.size())
            {
                break;
            }
            qsizetype end = start;
            while (end < draft.text.size() && !draft.text[end].isSpace())
            {
                ++end;
            }

            const auto token = draft.text.sliced(start, end - start);
            const auto it = resolutions.constFind(token);
            constexpr QStringView sevenTvEmoteUrlPrefix{
                u"https://7tv.app/emotes/"};
            const bool isFullSevenTvEmoteUrl =
                token.startsWith(sevenTvEmoteUrlPrefix, Qt::CaseInsensitive) &&
                token.size() > sevenTvEmoteUrlPrefix.size();
            if (!isFullSevenTvEmoteUrl && it != resolutions.cend() &&
                it->candidate)
            {
                draft.emotes.push_back({
                    .identity = it->candidate->identity,
                    .insertionText = it->candidate->insertionText,
                    .start = start,
                    .length = end - start,
                    .inferred = true,
                    .availability = it->candidate->availability,
                    .availabilityAlternatives =
                        it->candidate->availabilityAlternatives,
                    .identityAlternatives = it->candidate->identityAlternatives,
                });
            }
            start = end;
        }
        return draft;
    }

    /// Adds provenance for untracked whole-word tokens while preserving exact
    /// completion/picker selections on their original ranges.
    static MessageDraft reconstructUntracked(
        MessageDraft draft,
        std::span<const DraftEmoteCandidate> candidates)
    {
        auto inferred = reconstruct(draft.text, candidates);
        for (auto &occurrence : inferred.emotes)
        {
            const auto inferredEnd = occurrence.start + occurrence.length;
            const bool overlapsTracked =
                std::ranges::any_of(draft.emotes, [&](const auto &tracked) {
                    if (tracked.start < 0 || tracked.length <= 0 ||
                        tracked.start > draft.text.size() ||
                        tracked.length > draft.text.size() - tracked.start)
                    {
                        return false;
                    }
                    const auto trackedEnd = tracked.start + tracked.length;
                    return tracked.start < inferredEnd &&
                           occurrence.start < trackedEnd;
                });
            if (!overlapsTracked)
            {
                draft.emotes.push_back(std::move(occurrence));
            }
        }
        std::ranges::sort(draft.emotes, {}, &DraftEmoteOccurrence::start);
        return draft;
    }
};

enum class MessageDraftRejectionCode {
    NotWritable,
    NotAuthenticated,
    TooLong,
    UnsupportedEmote,
    ProviderConstraint,
    MalformedEmote,
    InvalidProvenance,
};

struct MessageDraftRejection {
    MessageDraftRejectionCode code;
    QString detail;
    std::optional<DraftEmoteIdentity> emote;
};

struct MessageSendContext {
    QString platform;
    QString channelID;
    QString accountID;
    bool writable = false;
    bool authenticated = false;
    /// Zero means that the provider has not supplied a local length limit.
    qsizetype maxMessageLength = 0;
    QStringList providerConstraints;
    /// When true, emoteCapabilities is an authoritative snapshot. An empty
    /// vector then means that no emotes are currently available.
    bool emoteCapabilitiesComplete = false;
    std::vector<DraftEmoteCapability> emoteCapabilities;
};

/// Builds the union dictionary used to reconstruct untracked tokens. Duplicate
/// capabilities are intentionally retained here; MessageDraft::reconstruct
/// combines them without losing the identity/availability pairing.
inline std::vector<DraftEmoteCandidate> messageDraftCandidates(
    std::span<const MessageSendContext> contexts)
{
    size_t count = 0;
    for (const auto &context : contexts)
    {
        count += context.emoteCapabilities.size();
    }

    std::vector<DraftEmoteCandidate> candidates;
    candidates.reserve(count);
    for (const auto &context : contexts)
    {
        for (const auto &capability : context.emoteCapabilities)
        {
            candidates.push_back({
                .identity = capability.identity,
                .insertionText = capability.insertionText,
                .availability = capability.availability,
                .availabilityAlternatives = {capability.availability},
            });
        }
    }
    return candidates;
}

struct MessageDraftEvaluation {
    bool sendable = false;
    /// Ordinary typed emote-like tokens may be sent as unchanged text when a
    /// destination cannot render them. Every other rejection remains hard.
    bool sendableWithInferredEmoteFallback = false;
    size_t supportedEmoteOccurrences = 0;
    size_t unsupportedInferredEmoteOccurrences = 0;
    std::vector<MessageDraftRejection> rejections;
};

inline MessageDraftEvaluation evaluateMessageDraft(
    const MessageDraft &draft, const MessageSendContext &context,
    QStringView sendText)
{
    MessageDraftEvaluation result;
    bool hasHardRejection = false;
    auto reject = [&result, &hasHardRejection](
                      MessageDraftRejectionCode code, QString detail,
                      std::optional<DraftEmoteIdentity> emote = std::nullopt,
                      bool hard = true) {
        result.rejections.push_back({.code = code,
                                     .detail = std::move(detail),
                                     .emote = std::move(emote)});
        hasHardRejection |= hard;
    };

    if (!context.writable)
    {
        reject(MessageDraftRejectionCode::NotWritable,
               QStringLiteral("The destination is not writable."));
    }
    if (!draft.provenanceValid)
    {
        reject(MessageDraftRejectionCode::InvalidProvenance,
               QStringLiteral(
                   "An inserted emote lost its stable provenance; reselect it."));
    }
    if (!context.authenticated)
    {
        reject(MessageDraftRejectionCode::NotAuthenticated,
               QStringLiteral(
                   "The destination requires an authenticated account."));
    }

    if (context.maxMessageLength > 0 &&
        sendText.size() > context.maxMessageLength)
    {
        reject(MessageDraftRejectionCode::TooLong,
               QStringLiteral(
                   "The draft exceeds the provider's message length limit."));
    }

    for (const auto &occurrence : draft.emotes)
    {
        const bool malformed =
            occurrence.identity.provider.isEmpty() ||
            occurrence.identity.id.string.isEmpty() || occurrence.length <= 0 ||
            occurrence.start < 0 || occurrence.start > draft.text.size() ||
            occurrence.length > draft.text.size() - occurrence.start ||
            draft.text.sliced(occurrence.start, occurrence.length) !=
                occurrence.insertionText ||
            std::ranges::any_of(
                occurrence.identityAlternatives, [](const auto &alternative) {
                    return alternative.identity.provider.isEmpty() ||
                           alternative.identity.id.string.isEmpty();
                });
        if (malformed)
        {
            reject(
                MessageDraftRejectionCode::MalformedEmote,
                QStringLiteral("The draft contains invalid emote provenance."),
                occurrence.identity);
            continue;
        }

        const auto availabilityMatches = [&context](
                                             const auto &availability) {
            if (!availability.known)
            {
                return false;
            }
            const bool platformMatches =
                availability.platform.isEmpty() ||
                availability.platform.compare(context.platform,
                                              Qt::CaseInsensitive) == 0;
            const bool channelMatches =
                !availability.channelID ||
                *availability.channelID == context.channelID;
            const bool accountMatches =
                !availability.accountID ||
                (!context.accountID.isEmpty() &&
                 *availability.accountID == context.accountID);
            return platformMatches && channelMatches && accountMatches;
        };

        const auto variantAvailable = [&](const auto &identity,
                                          const auto &availability,
                                          const auto &alternatives) {
            const bool declaredAvailable = alternatives.empty()
                                               ? availabilityMatches(
                                                     availability)
                                               : std::ranges::any_of(
                                                     alternatives,
                                                     availabilityMatches);
            if (!declaredAvailable)
            {
                return false;
            }
            if (!context.emoteCapabilitiesComplete)
            {
                return true;
            }
            return std::ranges::any_of(
                context.emoteCapabilities, [&](const auto &capability) {
                    return capability.identity == identity &&
                           capability.insertionText ==
                               occurrence.insertionText &&
                           availabilityMatches(capability.availability);
                });
        };

        bool available = variantAvailable(
            occurrence.identity, occurrence.availability,
            occurrence.availabilityAlternatives);
        if (!available)
        {
            available = std::ranges::any_of(
                occurrence.identityAlternatives,
                [&](const auto &alternative) {
                    return variantAvailable(
                        alternative.identity, alternative.availability,
                        alternative.availabilityAlternatives);
                });
        }

        if (!available)
        {
            if (occurrence.inferred)
            {
                ++result.unsupportedInferredEmoteOccurrences;
            }
            reject(
                MessageDraftRejectionCode::UnsupportedEmote,
                QStringLiteral("The emote is unavailable in this destination."),
                occurrence.identity, !occurrence.inferred);
        }
        else
        {
            ++result.supportedEmoteOccurrences;
        }
    }

    for (const auto &constraint : context.providerConstraints)
    {
        reject(MessageDraftRejectionCode::ProviderConstraint, constraint);
    }

    result.sendable = result.rejections.empty();
    result.sendableWithInferredEmoteFallback = !hasHardRejection;
    return result;
}

inline MessageDraftEvaluation evaluateMessageDraft(
    const MessageDraft &draft, const MessageSendContext &context)
{
    return evaluateMessageDraft(draft, context, QStringView{draft.text});
}

}  // namespace chatterino
