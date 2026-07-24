// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/EmoteSource.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cassert>
#include <ranges>

namespace chatterino::completion {

namespace {

QString stableEmoteID(const EmotePtr &emote)
{
    if (emote && !emote->id.string.isEmpty())
    {
        return emote->id.string;
    }
    // A display name is not a provider-stable identity. The row may still be
    // shown, but selecting it is marked unresolved by the typed path.
    return {};
}

void addEmotes(std::vector<EmoteItem> &out, const EmoteMap &map,
               const QString &providerName, const QString &providerID,
               DraftEmoteAvailability availability,
               std::optional<size_t> childIndex = std::nullopt,
               const QString &availabilityContext = {})
{
    for (auto &&emote : map)
    {
        EmoteItem item{
            .emote = emote.second,
            .searchName = emote.first.string,
            .tabCompletionName = emote.first.string,
            .displayName = emote.second->name.string,
            .providerName = providerName,
            .isEmoji = false,
            .identity =
                {
                    .provider = providerID,
                    .id = EmoteId{stableEmoteID(emote.second)},
                },
            .availabilities = {availability},
        };
        if (childIndex)
        {
            item.availableInChildren.push_back(*childIndex);
            item.availabilityContexts.push_back(availabilityContext);
        }
        out.push_back(std::move(item));
    }
}

void addEmojis(std::vector<EmoteItem> &out, const std::vector<EmojiPtr> &map,
               std::optional<size_t> childIndex = std::nullopt,
               const QString &availabilityContext = {})
{
    for (const auto &emoji : map)
    {
        for (auto &&shortCode : emoji->shortCodes)
        {
            EmoteItem item{
                .emote = emoji->emote,
                .searchName = shortCode,
                .tabCompletionName = QStringLiteral(":%1:").arg(shortCode),
                .displayName = shortCode,
                .providerName = QStringLiteral("Emoji"),
                .isEmoji = true,
                .identity =
                    {
                        .provider = QStringLiteral("emoji"),
                        .id = EmoteId{emoji->unifiedCode},
                    },
                .availabilities = {DraftEmoteAvailability{}},
            };
            if (childIndex)
            {
                item.availableInChildren.push_back(*childIndex);
                item.availabilityContexts.push_back(availabilityContext);
            }
            out.push_back(std::move(item));
        }
    };
}

QString emoteKey(const EmoteItem &item)
{
    const auto provider = item.identity.provider.isEmpty()
                              ? item.providerName
                              : item.identity.provider;
    const auto id = item.identity.id.string.isEmpty()
                        ? item.displayName
                        : item.identity.id.string;
    return provider + QChar{0x1f} + id + QChar{0x1f} +
           item.tabCompletionName;
}

bool containsChild(const std::vector<size_t> &children, size_t child)
{
    return std::ranges::find(children, child) != children.end();
}

void appendUnique(std::vector<size_t> &values, size_t value)
{
    if (!containsChild(values, value))
    {
        values.push_back(value);
    }
}

void appendUnique(std::vector<QString> &values, const QString &value)
{
    if (std::ranges::find(values, value) == values.end())
    {
        values.push_back(value);
    }
}

void appendUnique(std::vector<DraftEmoteAvailability> &values,
                  const DraftEmoteAvailability &value)
{
    if (std::ranges::find(values, value) == values.end())
    {
        values.push_back(value);
    }
}

}  // namespace

std::optional<DraftEmoteCandidate> draftCandidateFromEmoteItem(
    const EmoteItem &item)
{
    if (item.identity.provider.isEmpty() || item.identity.id.string.isEmpty() ||
        item.tabCompletionName.isEmpty() || item.availabilities.empty() ||
        std::ranges::any_of(item.availabilities,
                            [](const auto &scope) { return !scope.known; }))
    {
        return std::nullopt;
    }

    return DraftEmoteCandidate{
        .identity = item.identity,
        .insertionText = item.tabCompletionName,
        .availability = item.availabilities.front(),
        .availabilityAlternatives = item.availabilities,
    };
}

std::vector<EmoteItem> mergeMultiChannelEmoteItems(
    std::vector<EmoteItem> items)
{
    std::vector<EmoteItem> merged;
    merged.reserve(items.size());
    QHash<QString, size_t> positions;
    positions.reserve(static_cast<qsizetype>(items.size()));

    for (auto &item : items)
    {
        const auto key = emoteKey(item);
        const auto existing = positions.constFind(key);
        if (existing == positions.cend())
        {
            positions.insert(key, merged.size());
            merged.push_back(std::move(item));
            continue;
        }

        auto &destination = merged[*existing];
        for (const auto child : item.availableInChildren)
        {
            appendUnique(destination.availableInChildren, child);
        }
        for (const auto &context : item.availabilityContexts)
        {
            appendUnique(destination.availabilityContexts, context);
        }
        for (const auto &availability : item.availabilities)
        {
            appendUnique(destination.availabilities, availability);
        }
    }

    return merged;
}

std::vector<EmoteItem> filterMultiChannelEmoteItems(
    std::span<const EmoteItem> items, const MessageDraft &draft,
    std::span<const MessageSendContext> destinations, size_t activeChild)
{
    auto evaluationDraft = draft;
    if (draft.destinationPlatformOverride)
    {
        // Existing selected emotes become ordinary rendered text when the
        // operator explicitly changes platform. The prospective completion
        // item is still filtered through availableInChildren below.
        evaluationDraft.emotes.clear();
    }

    std::vector<bool> viable(destinations.size(), false);
    for (size_t child = 0; child < destinations.size(); ++child)
    {
        const bool platformAllowed =
            !draft.destinationPlatformOverride ||
            destinations[child].platform.compare(
                *draft.destinationPlatformOverride, Qt::CaseInsensitive) == 0;
        viable[child] =
            platformAllowed && chatterino::evaluateMessageDraft(
                                   evaluationDraft, destinations[child])
                                   .sendable;
    }

    std::vector<EmoteItem> filtered;
    filtered.reserve(items.size());
    for (const auto &item : items)
    {
        EmoteItem candidate = item;
        candidate.compatibleChannelCount = 0;
        candidate.primaryChannelCompatible = false;
        for (const auto child : item.availableInChildren)
        {
            if (child < viable.size() && viable[child])
            {
                ++candidate.compatibleChannelCount;
                candidate.primaryChannelCompatible |= child == activeChild;
            }
        }
        if (candidate.compatibleChannelCount != 0)
        {
            filtered.push_back(std::move(candidate));
        }
    }
    return filtered;
}

void prioritizeMultiChannelEmoteItems(std::vector<EmoteItem> &items)
{
    std::stable_sort(items.begin(), items.end(),
                     [](const EmoteItem &left, const EmoteItem &right) {
                         if (left.primaryChannelCompatible !=
                             right.primaryChannelCompatible)
                         {
                             return left.primaryChannelCompatible;
                         }
                         return left.compatibleChannelCount >
                                right.compatibleChannelCount;
                     });
}

EmoteSource::EmoteSource(const Channel *channel,
                         std::unique_ptr<EmoteStrategy> strategy,
                         ActionCallback callback)
    : channel_(channel)
    , strategy_(std::move(strategy))
    , callback_(std::move(callback))
{
    this->initializeFromChannel(channel);
}

void EmoteSource::setInputContext(const QString &input)
{
    this->draft_ = MessageDraft::fromPlainText(input);
}

void EmoteSource::setInputContext(const MessageDraft &input)
{
    this->draft_ = input;
}

void EmoteSource::update(const QString &query)
{
    this->output_.clear();
    this->resolutionItems_.clear();
    if (this->strategy_)
    {
        if (!this->multiChannel_ && this->channel_->isRumbleChannel())
        {
            // The public catalog and account eligibility arrive
            // asynchronously. Refresh from the in-memory snapshot on each
            // query; this path performs no network I/O.
            this->initializeFromChannel(this->channel_);
        }
        if (this->multiChannel_)
        {
            // Emote/account/child state is in-memory and can change while the
            // split remains alive. Refreshing here performs no network I/O.
            this->initializeFromChannel(this->channel_);

            const auto *multi = dynamic_cast<const MultiChannel *>(
                this->channel_);
            assert(multi != nullptr);
            std::vector<MessageSendContext> destinations;
            destinations.reserve(multi->channels().size());
            for (const auto &child : multi->channels())
            {
                destinations.push_back(child.channel->messageSendContext());
            }

            const auto dictionary = messageDraftCandidates(destinations);
            const auto reconstructed =
                MessageDraft::reconstructUntracked(this->draft_, dictionary);
            const auto automaticDestination =
                multi->previewMessageDraftDestination(
                    reconstructed, reconstructed.text,
                    multi->activeChannelIndex(),
                    MultiChannelRoutePolicy::CompatibleFallback);
            // Completion priority follows the same current automatic
            // destination as preview and submission. The persisted active
            // child remains unchanged, so an unavailable first child can
            // become primary again without rewriting the layout.
            const auto effectivePrimary =
                automaticDestination.destinationIndex.value_or(
                    destinations.size());

            this->resolutionItems_ = filterMultiChannelEmoteItems(
                this->items_, reconstructed, destinations, effectivePrimary);
            this->strategy_->apply(this->resolutionItems_, this->output_,
                                   query);
            prioritizeMultiChannelEmoteItems(this->output_);
            return;
        }
        this->strategy_->apply(this->items_, this->output_, query);
    }
}

void EmoteSource::addToListModel(GenericListModel &model, size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const EmoteItem &e) {
                          const auto candidate =
                              draftCandidateFromEmoteItem(e);
                          return std::make_unique<InputCompletionItem>(
                              e.emote, e.displayName + " - " + e.providerName,
                              [callback = this->callback_, candidate](
                                  const QString &text) {
                                  if (callback)
                                  {
                                      callback(candidate
                                                   ? candidate->insertionText
                                                   : text,
                                               candidate,
                                               !candidate.has_value());
                                  }
                              });
                      });
}

void EmoteSource::addToStringList(QStringList &list, size_t maxCount,
                                  bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount, [](const EmoteItem &e) {
        return e.tabCompletionName + " ";
    });
}

void EmoteSource::addToStringCompletions(
    std::vector<StringCompletion> &list, size_t maxCount,
    bool /* isFirstWord */) const
{
    struct Resolution {
        std::optional<DraftEmoteCandidate> candidate;
        bool ambiguous = false;
    };

    // Build one authoritative resolution per rendered text from the filtered
    // output. Same-text provider identities are exact destination alternatives
    // for tab completion; candidates removed by the current draft must not
    // poison the remaining row.
    QHash<QString, Resolution> resolutions;
    const auto &resolutionItems =
        this->multiChannel_ ? this->resolutionItems_ : this->items_;
    resolutions.reserve(static_cast<qsizetype>(resolutionItems.size()));
    for (const auto &available : resolutionItems)
    {
        auto alternative = draftCandidateFromEmoteItem(available);
        auto &resolution = resolutions[available.tabCompletionName];
        if (!alternative)
        {
            resolution.candidate.reset();
            resolution.ambiguous = true;
            continue;
        }
        if (resolution.ambiguous)
        {
            continue;
        }
        if (!resolution.candidate)
        {
            resolution.candidate = std::move(alternative);
            continue;
        }
        mergeDraftEmoteCandidate(*resolution.candidate, *alternative);
    }

    const auto count = maxCount == 0
                           ? this->output_.size()
                           : std::min(maxCount, this->output_.size());
    list.reserve(list.size() + count);
    for (const auto &item : this->output_ | std::views::take(count))
    {
        std::optional<DraftEmoteCandidate> candidate;
        const auto resolution = resolutions.constFind(item.tabCompletionName);
        if (resolution != resolutions.cend() && !resolution->ambiguous)
        {
            candidate = resolution->candidate;
        }
        list.push_back({
            .text = item.tabCompletionName + " ",
            .emote = std::move(candidate),
            .emoteLike = true,
        });
    }
}

void EmoteSource::initializeFromChannel(const Channel *channel)
{
    auto *app = getApp();

    std::vector<EmoteItem> emotes;
    const auto addFromChannel = [&](const Channel *source,
                                    std::optional<size_t> childIndex,
                                    const QString &availabilityContext) {
        const auto sendContext = source->messageSendContext();
        const DraftEmoteAvailability channelScope{
            .platform = sendContext.platform,
            .channelID = sendContext.channelID,
        };
        const DraftEmoteAvailability channelAccountScope{
            .platform = sendContext.platform,
            .channelID = sendContext.channelID,
            .accountID = sendContext.accountID,
        };
        const DraftEmoteAvailability accountScope{
            .platform = sendContext.platform,
            .accountID = sendContext.accountID,
        };
        const DraftEmoteAvailability platformScope{
            .platform = sendContext.platform,
        };
        const auto *tc = dynamic_cast<const TwitchChannel *>(source);
        // returns true also for special Twitch channels (/live, /mentions,
        // /whispers, etc.)
        if (source->isTwitchChannel() && tc)
        {
            if (auto twitch = tc->localTwitchEmotes())
            {
                addEmotes(emotes, *twitch, "Local Twitch Emotes", "twitch",
                          channelAccountScope, childIndex,
                          availabilityContext);
            }

            auto user = getApp()->getAccounts()->twitch.getCurrent();
            addEmotes(emotes, **user->accessEmotes(), "Twitch Emote",
                      "twitch", accountScope, childIndex,
                      availabilityContext);

            for (const auto &map :
                 app->getSeventvPersonalEmotes()->getEmoteSetsForTwitchUser(
                     app->getAccounts()->twitch.getCurrent()->getUserId()))
            {
                addEmotes(emotes, *map, "Personal 7TV", "7tv", accountScope,
                          childIndex, availabilityContext);
            }

            // TODO extract "Channel {BetterTTV,7TV,FrankerFaceZ}" text into a
            // #define.
            if (auto bttv = tc->bttvEmotes())
            {
                addEmotes(emotes, *bttv, "Channel BetterTTV", "bttv",
                          channelScope, childIndex, availabilityContext);
            }
            if (auto ffz = tc->ffzEmotes())
            {
                addEmotes(emotes, *ffz, "Channel FrankerFaceZ", "ffz",
                          channelScope, childIndex, availabilityContext);
            }
            if (auto seventv = tc->seventvEmotes())
            {
                addEmotes(emotes, *seventv, "Channel 7TV", "7tv",
                          channelScope, childIndex, availabilityContext);
            }
        }

        const auto *kickChannel = dynamic_cast<const KickChannel *>(source);
        if (kickChannel)
        {
            const auto list =
                app->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                    app->getAccounts()->kick.current()->userID());
            for (const auto &map : list)
            {
                addEmotes(emotes, *map, "Personal 7TV", "7tv", accountScope,
                          childIndex, availabilityContext);
            }

            addEmotes(emotes, *kickChannel->seventvEmotes(), "Channel 7TV",
                      "7tv", channelScope, childIndex, availabilityContext);
            addEmotes(emotes, *getApp()->getKickChatServer()->globalEmotes(),
                      "Kick Emote", "kick", platformScope, childIndex,
                      availabilityContext);
        }

        const auto *rumbleChannel =
            dynamic_cast<const RumbleChannel *>(source);
        if (rumbleChannel)
        {
            for (const auto &entry : rumbleChannel->availableEmotes())
            {
                EmoteItem item{
                    .emote = entry.emote,
                    .searchName = entry.searchName,
                    .tabCompletionName = entry.insertionText,
                    .displayName = entry.searchName,
                    .providerName = QStringLiteral("Rumble Emote"),
                    .isEmoji = false,
                    .identity = {
                        .provider = QStringLiteral("rumble"),
                        .id = entry.emote->id,
                    },
                    .availabilities = {{
                        .platform = QStringLiteral("rumble"),
                        .channelID =
                            entry.scope == rumble::EmoteScope::Channel
                                ? std::optional<QString>{sendContext.channelID}
                                : std::nullopt,
                        .accountID = sendContext.accountID,
                    }},
                };
                if (childIndex)
                {
                    item.availableInChildren.push_back(*childIndex);
                    item.availabilityContexts.push_back(availabilityContext);
                }
                emotes.push_back(std::move(item));
            }
        }

        if (source->isTwitchOrKickChannel())
        {
            if (auto bttvG = app->getBttvEmotes()->emotes())
            {
                addEmotes(emotes, *bttvG, "Global BetterTTV", "bttv",
                          platformScope, childIndex, availabilityContext);
            }
            if (auto ffzG = app->getFfzEmotes()->emotes())
            {
                addEmotes(emotes, *ffzG, "Global FrankerFaceZ", "ffz",
                          platformScope, childIndex, availabilityContext);
            }
            if (auto seventvG = app->getSeventvEmotes()->globalEmotes())
            {
                addEmotes(emotes, *seventvG, "Global 7TV", "7tv",
                          platformScope, childIndex, availabilityContext);
            }
        }

        addEmojis(emotes, app->getEmotes()->getEmojis()->getEmojis(),
                  childIndex, availabilityContext);
    };

    if (const auto *multi = dynamic_cast<const MultiChannel *>(channel))
    {
        this->multiChannel_ = true;
        size_t childIndex = 0;
        for (const auto &child : multi->channels())
        {
            const auto context = child.channel->messageSendContext();
            const auto label =
                QStringLiteral("%1/%2")
                    .arg(context.platform, child.channel->getDisplayName());
            addFromChannel(child.channel.get(), childIndex, label);
            ++childIndex;
        }
        emotes = mergeMultiChannelEmoteItems(std::move(emotes));
        for (auto &emote : emotes)
        {
            if (!emote.availabilityContexts.empty())
            {
                QStringList contexts;
                contexts.reserve(static_cast<qsizetype>(
                    emote.availabilityContexts.size()));
                for (const auto &context : emote.availabilityContexts)
                {
                    contexts.push_back(context);
                }
                emote.providerName += QStringLiteral(" • ") +
                                      contexts.join(QStringLiteral(", "));
            }
        }
    }
    else
    {
        this->multiChannel_ = false;
        addFromChannel(channel, std::nullopt, {});
    }

    this->items_ = std::move(emotes);
}

const std::vector<EmoteItem> &EmoteSource::output() const
{
    return this->output_;
}

EmoteCandidateResolution EmoteSource::resolveCandidate(
    const EmotePtr &emote, QStringView insertionText) const
{
    EmoteCandidateResolution result;
    for (const auto &item : this->items_)
    {
        if (item.emote != emote || item.tabCompletionName != insertionText)
        {
            continue;
        }

        auto candidate = draftCandidateFromEmoteItem(item);
        if (!candidate)
        {
            result.ambiguous = true;
            result.candidate.reset();
            continue;
        }
        if (!result.candidate)
        {
            if (!result.ambiguous)
            {
                result.candidate = std::move(candidate);
            }
            continue;
        }
        if (result.candidate->identity != candidate->identity)
        {
            result.candidate.reset();
            result.ambiguous = true;
            continue;
        }
        for (const auto &availability :
             candidate->availabilityAlternatives)
        {
            appendUnique(result.candidate->availabilityAlternatives,
                         availability);
        }
    }
    if (!result.candidate)
    {
        result.ambiguous = true;
    }
    return result;
}

}  // namespace chatterino::completion
