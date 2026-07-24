// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "controllers/completion/sources/Source.hpp"
#include "controllers/completion/strategies/Strategy.hpp"
#include "messages/Emote.hpp"
#include "messages/MessageDraft.hpp"

#include <QString>
#include <QStringView>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace chatterino::completion {

struct EmoteItem {
    /// Emote image to show in input popup
    EmotePtr emote{};
    /// Name to check completion queries against
    QString searchName{};
    /// Name to insert into split input upon tab completing
    QString tabCompletionName{};
    /// Display name within input popup
    QString displayName{};
    /// Emote provider name for input popup
    QString providerName{};
    /// Whether emote is emoji
    bool isEmoji{};
    /// Provider-stable identity used to preserve completion provenance.
    DraftEmoteIdentity identity{};
    /// Exact scopes represented by this row. Aggregate rows can carry one
    /// alternative per child without widening channel/account constraints.
    std::vector<DraftEmoteAvailability> availabilities{};
    /// Multi-channel children where this exact identity can be used.
    std::vector<size_t> availableInChildren{};
    /// Human-readable child/provider contexts shown for ambiguous names.
    std::vector<QString> availabilityContexts{};
    /// Dynamic compatibility tier for the current draft.
    bool primaryChannelCompatible{};
    size_t compatibleChannelCount{};
};

struct EmoteCandidateResolution {
    std::optional<DraftEmoteCandidate> candidate;
    bool ambiguous = false;
};

std::optional<DraftEmoteCandidate> draftCandidateFromEmoteItem(
    const EmoteItem &item);

/// Deduplicate the same provider identity exposed by multiple children while
/// keeping same-name, different-identity emotes separate.
std::vector<EmoteItem> mergeMultiChannelEmoteItems(
    std::vector<EmoteItem> items);

/// Keep candidates that leave at least one child capable of sending the
/// accumulated draft. The returned items retain their original order and carry
/// compatibility tiers for prioritizeMultiChannelEmoteItems().
std::vector<EmoteItem> filterMultiChannelEmoteItems(
    std::span<const EmoteItem> items, const MessageDraft &draft,
    std::span<const MessageSendContext> destinations, size_t activeChild);

/// Stable tiering: active-child compatible first, then wider child coverage.
/// Existing classic/smart strategy order is preserved inside each tier.
void prioritizeMultiChannelEmoteItems(std::vector<EmoteItem> &items);

class EmoteSource : public Source
{
public:
    using ActionCallback = std::function<void(
        const QString &, const std::optional<DraftEmoteCandidate> &, bool)>;
    using EmoteStrategy = Strategy<EmoteItem>;

    /// @brief Initializes a source for EmoteItems from the given channel
    /// @param channel Channel to initialize emotes from
    /// @param strategy Strategy to apply
    /// @param callback ActionCallback to invoke upon InputCompletionItem selection.
    /// See InputCompletionItem::action(). Can be nullptr.
    EmoteSource(const Channel *channel, std::unique_ptr<EmoteStrategy> strategy,
                ActionCallback callback = nullptr);

    void setInputContext(const QString &input) override;
    void setInputContext(const MessageDraft &input) override;
    void update(const QString &query) override;
    void addToListModel(GenericListModel &model,
                        size_t maxCount = 0) const override;
    void addToStringList(QStringList &list, size_t maxCount = 0,
                         bool isFirstWord = false) const override;
    void addToStringCompletions(std::vector<StringCompletion> &list,
                                size_t maxCount = 0,
                                bool isFirstWord = false) const override;

    const std::vector<EmoteItem> &output() const;
    EmoteCandidateResolution resolveCandidate(
        const EmotePtr &emote, QStringView insertionText) const;

private:
    void initializeFromChannel(const Channel *channel);

    const Channel *channel_{};
    std::unique_ptr<EmoteStrategy> strategy_;
    ActionCallback callback_;
    MessageDraft draft_{};
    bool multiChannel_{};

    std::vector<EmoteItem> items_{};
    /// Exact candidates compatible with the current draft before a strategy
    /// potentially deduplicates equal rendered names.
    std::vector<EmoteItem> resolutionItems_{};
    std::vector<EmoteItem> output_{};
};

}  // namespace chatterino::completion
