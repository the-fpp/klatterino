// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageDraft.hpp"

#include <QStringList>

#include <optional>
#include <utility>
#include <vector>

namespace chatterino {
class GenericListModel;
}  // namespace chatterino

namespace chatterino::completion {

/// One tab-completion row plus optional exact emote provenance. emoteLike is
/// true when choosing this row must either record that candidate or fail
/// conservatively because another source collided with its rendered text.
struct StringCompletion {
    QString text;
    std::optional<DraftEmoteCandidate> emote;
    bool emoteLike = false;
};

/// @brief A Source represents a source for generating completion suggestions.
///
/// The source can be queried to update its suggestions and then write the completion
/// suggestions to a GenericListModel or QStringList depending on the consumer's
/// requirements.
///
/// For example, consider providing emotes for completion. The Source instance
/// initialized with every available emote in the channel (including  global
/// emotes). As the user updates their query by typing, the suggestions are
/// refined and the output model is updated.
class Source
{
public:
    virtual ~Source() = default;

    /// @brief Supplies surrounding input state used by context-sensitive
    /// sources. Sources that only depend on the current query may ignore it.
    virtual void setInputContext(const QString & /* input */)
    {
    }

    /// Typed input context for consumers that track exact emote provenance.
    /// The adapter preserves existing plain-text consumers such as legacy tab
    /// completion without letting context-sensitive sources guess identities
    /// from rendered tokens.
    virtual void setInputContext(const MessageDraft &input)
    {
        this->setInputContext(input.text);
    }

    /// @brief Updates the internal completion suggestions for the given query
    /// @param query Query to complete against
    virtual void update(const QString &query) = 0;

    /// @brief Appends the internal completion suggestions to a GenericListModel
    /// @param model GenericListModel to add suggestions to
    /// @param maxCount Maximum number of suggestions. Zero indicates unlimited.
    virtual void addToListModel(GenericListModel &model,
                                size_t maxCount = 0) const = 0;

    /// @brief Appends the internal completion suggestions to a QStringList
    /// @param list QStringList to add suggestions to
    /// @param maxCount Maximum number of suggestions. Zero indicates unlimited.
    /// @param isFirstWord Whether the completion is the first word in the input
    virtual void addToStringList(QStringList &list, size_t maxCount = 0,
                                 bool isFirstWord = false) const = 0;

    /// Typed counterpart used by tab completion. Non-emote sources inherit the
    /// plain-text adapter; emote and unified sources preserve provenance.
    virtual void addToStringCompletions(
        std::vector<StringCompletion> &list, size_t maxCount = 0,
        bool isFirstWord = false) const
    {
        QStringList strings;
        this->addToStringList(strings, maxCount, isFirstWord);
        list.reserve(list.size() + static_cast<size_t>(strings.size()));
        for (auto &string : strings)
        {
            list.push_back({.text = std::move(string)});
        }
    }
};

};  // namespace chatterino::completion
