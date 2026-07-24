// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/TabCompletionModel.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/sources/UnifiedSource.hpp"
#include "controllers/completion/sources/UserSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "controllers/completion/strategies/ClassicUserStrategy.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "controllers/completion/strategies/SmartEmoteStrategy.hpp"
#include "controllers/plugins/LuaUtilities.hpp"
#include "controllers/plugins/Plugin.hpp"
#include "controllers/plugins/PluginController.hpp"
#include "singletons/Settings.hpp"
#include "util/MultiChannel.hpp"

#include <QHash>

namespace chatterino {

TabCompletionModel::TabCompletionModel(Channel &channel, QObject *parent)
    : QStringListModel(parent)
    , channel_(channel)
{
}

void TabCompletionModel::updateResults(const QString &query,
                                       const QString &fullTextContent,
                                       int cursorPosition, bool isFirstWord)
{
    this->updateResults(query, MessageDraft::fromPlainText(fullTextContent),
                        cursorPosition, isFirstWord);
}

void TabCompletionModel::updateResults(const QString &query,
                                       const MessageDraft &draft,
                                       int cursorPosition, bool isFirstWord)
{
    this->updateSourceFromQuery(query, isFirstWord);

    if (this->source_)
    {
        this->source_->setInputContext(draft);
        this->source_->update(query);

        std::vector<completion::StringCompletion> suggestions;
#ifdef CHATTERINO_HAVE_PLUGINS
        // Try plugins first
        bool done{};
        QStringList pluginResults;
        std::tie(done, pluginResults) =
            getApp()->getPlugins()->updateCustomCompletions(
                query, draft.text, cursorPosition, isFirstWord);
        suggestions.reserve(static_cast<size_t>(pluginResults.size()));
        for (auto &pluginResult : pluginResults)
        {
            suggestions.push_back({.text = std::move(pluginResult)});
        }
        if (done)
        {
            // A plugin-owned row is always plain text. It must never inherit a
            // hidden native emote's provenance merely because text matches.
            this->source_.reset();
        }
#endif
        if (this->source_)
        {
            this->source_->addToStringCompletions(suggestions, 0,
                                                  isFirstWord);
        }

        QStringList results;
        std::vector<Selection> selections;
        QHash<QString, qsizetype> rows;
        rows.reserve(static_cast<qsizetype>(suggestions.size()));
        for (auto &suggestion : suggestions)
        {
            // Sources differ on whether they include the trailing separator.
            // Treat that invisible difference as the same rendered row for
            // provenance-collision purposes.
            const auto collisionKey = suggestion.text.trimmed();
            const auto existing = rows.constFind(collisionKey);
            if (existing == rows.cend())
            {
                const auto row = results.size();
                rows.insert(collisionKey, row);
                results.push_back(std::move(suggestion.text));
                selections.push_back({
                    .emote = std::move(suggestion.emote),
                    .provenanceAmbiguous =
                        suggestion.emoteLike && !suggestion.emote,
                });
                continue;
            }

            auto &selection = selections[static_cast<size_t>(*existing)];
            const bool sameCandidate =
                selection.emote && suggestion.emote &&
                *selection.emote == *suggestion.emote;
            if ((selection.emote || suggestion.emote ||
                 selection.provenanceAmbiguous || suggestion.emoteLike) &&
                !sameCandidate)
            {
                selection.emote.reset();
                selection.provenanceAmbiguous = true;
            }
        }
        this->selections_ = std::move(selections);
        this->setStringList(results);
        return;
    }

    this->selections_.clear();
    this->setStringList({});
}

TabCompletionModel::Selection TabCompletionModel::selectionForRow(int row) const
{
    if (row < 0 || static_cast<size_t>(row) >= this->selections_.size())
    {
        return {};
    }
    return this->selections_[static_cast<size_t>(row)];
}

TabCompletionModel::Selection TabCompletionModel::selectionForCompletion(
    const QString &completion) const
{
    // QCompleter::currentRow() indexes its filtered completion proxy, not this
    // source model. Completion strings are unique after updateResults(), so
    // resolve the selected proxy value back to its authoritative source row.
    return this->selectionForRow(this->stringList().indexOf(completion));
}

void TabCompletionModel::updateSourceFromQuery(const QString &query,
                                               bool isFirstWord)
{
    auto deducedKind = this->deduceSourceKind(query, isFirstWord);
    if (!deducedKind)
    {
        // unable to determine what kind of completion is occurring
        this->source_ = nullptr;
        return;
    }

    // Build source for new query
    this->source_ = this->buildSource(*deducedKind);
}

std::optional<TabCompletionModel::SourceKind>
    TabCompletionModel::deduceSourceKind(const QString &query,
                                         bool isFirstWord) const
{
    if (query.length() < 2 ||
        (!this->channel_.isTwitchOrKickChannel() &&
         !this->channel_.isRumbleChannel() &&
         dynamic_cast<const MultiChannel *>(&this->channel_) == nullptr))
    {
        return std::nullopt;
    }

    // Check for cases where we can definitively say what kind of completion is taking place.

    if (query.startsWith('@'))
    {
        return SourceKind::User;
    }
    else if (query.startsWith(':'))
    {
        return SourceKind::Emote;
    }
    else if (isFirstWord && (query.startsWith('/') || query.startsWith('.')))
    {
        return SourceKind::Command;
    }

    // At this point, we note that emotes can be completed without using a :
    // Therefore, we must also consider that the user could be completing an emote
    // OR a mention depending on their completion settings.

    if (isFirstWord)
    {
        if (getSettings()->userCompletionOnlyWithAt)
        {
            // All kinds but user are possible
            return SourceKind::EmoteCommand;
        }

        // Any kind is possible
        return SourceKind::EmoteUserCommand;
    }

    // We don't allow for mid-message command completions,
    // which means only emote or user tab completions are possible.

    if (getSettings()->userCompletionOnlyWithAt)
    {
        return SourceKind::Emote;
    }

    return SourceKind::EmoteUser;
}

std::unique_ptr<completion::Source> TabCompletionModel::buildSource(
    SourceKind kind) const
{
    switch (kind)
    {
        case SourceKind::Emote: {
            return this->buildEmoteSource();
        }
        case SourceKind::User: {
            return this->buildUserSource(true);  // Completing with @
        }
        case SourceKind::Command: {
            return this->buildCommandSource();
        }
        case SourceKind::EmoteUser: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildUserSource(false));

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteUserCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(
                this->buildUserSource(false));  // Not completing with @
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        default:
            return nullptr;
    }
}

std::unique_ptr<completion::Source> TabCompletionModel::buildEmoteSource() const
{
    if (getSettings()->useSmartEmoteCompletion)
    {
        return std::make_unique<completion::EmoteSource>(
            &this->channel_,
            std::make_unique<completion::SmartTabEmoteStrategy>());
    }

    return std::make_unique<completion::EmoteSource>(
        &this->channel_,
        std::make_unique<completion::ClassicTabEmoteStrategy>());
}

std::unique_ptr<completion::Source> TabCompletionModel::buildUserSource(
    bool prependAt) const
{
    return std::make_unique<completion::UserSource>(
        &this->channel_, std::make_unique<completion::ClassicUserStrategy>(),
        nullptr, prependAt);
}

std::unique_ptr<completion::Source> TabCompletionModel::buildCommandSource()
    const
{
    return std::make_unique<completion::CommandSource>(
        std::make_unique<completion::CommandStrategy>(true));
}

}  // namespace chatterino
