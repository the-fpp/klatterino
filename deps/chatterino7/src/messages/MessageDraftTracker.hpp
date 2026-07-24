// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageDraft.hpp"

#include <QStringView>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace chatterino {

/// Tracks explicitly selected emotes through QTextDocument::contentsChange
/// ranges. Multi-channel completion/routing can add union-dictionary
/// provenance to the untracked ranges of a snapshot without changing this
/// exact selection history.
class MessageDraftTracker
{
public:
    void contentsChanged(qsizetype position, qsizetype removed,
                         qsizetype added, QStringView currentText)
    {
        ++this->revision_;
        // A subsequent user/document edit gives a failed insertion a chance to
        // be corrected. A new failed typed insertion will set this again after
        // its synchronous contentsChange notification.
        this->invalidInsertion_ = false;

        if (position < 0 || removed < 0 || added < 0 ||
            position > currentText.size() ||
            added > currentText.size() - position ||
            removed > std::numeric_limits<qsizetype>::max() - position)
        {
            this->occurrences_.clear();
            this->unresolved_.clear();
            this->invalidInsertion_ = true;
            return;
        }

        const auto changeEnd = position + removed;
        const auto delta = added - removed;
        std::vector<DraftEmoteOccurrence> retained;
        retained.reserve(this->occurrences_.size());
        for (auto occurrence : this->occurrences_)
        {
            const auto occurrenceEnd = occurrence.start + occurrence.length;
            if (occurrenceEnd <= position)
            {
                // The edit is wholly after the selected token.
            }
            else if (occurrence.start >= changeEnd)
            {
                // The edit is wholly before the selected token.
                occurrence.start += delta;
            }
            else
            {
                // Any overlap, including an identical replacement, destroys
                // provenance. Equal rendered text must not inherit identity.
                continue;
            }

            if (this->matches(occurrence, currentText))
            {
                retained.push_back(std::move(occurrence));
            }
        }
        this->occurrences_ = std::move(retained);

        std::vector<Range> unresolved;
        unresolved.reserve(this->unresolved_.size());
        for (auto range : this->unresolved_)
        {
            const auto rangeEnd = range.start + range.length;
            if (rangeEnd <= position)
            {
                // The edit is wholly after the unresolved insertion.
            }
            else if (range.start >= changeEnd)
            {
                range.start += delta;
            }
            else
            {
                // Editing any part of the ambiguous insertion removes the
                // poison; text beside it does not.
                continue;
            }
            if (range.start >= 0 && range.length > 0 &&
                range.start <= currentText.size() &&
                range.length <= currentText.size() - range.start)
            {
                unresolved.push_back(range);
            }
        }
        this->unresolved_ = std::move(unresolved);
    }

    /// Records a typed completion/picker selection after the text insertion
    /// has completed. Returns false and poisons the current snapshot when the
    /// observed document range does not exactly match the selected candidate.
    bool recordSelection(qsizetype start, DraftEmoteCandidate candidate,
                         QStringView currentText)
    {
        DraftEmoteOccurrence occurrence{
            .identity = std::move(candidate.identity),
            .insertionText = std::move(candidate.insertionText),
            .start = start,
            .length = 0,
            .availability = std::move(candidate.availability),
            .availabilityAlternatives =
                std::move(candidate.availabilityAlternatives),
            .identityAlternatives =
                std::move(candidate.identityAlternatives),
        };
        occurrence.length = occurrence.insertionText.size();
        if (!this->matches(occurrence, currentText))
        {
            if (currentText.isEmpty())
            {
                this->invalidInsertion_ = true;
            }
            else
            {
                // We cannot prove which rendered characters came from the
                // failed typed insertion. Poison the current document until
                // the user edits it rather than guessing a provider range.
                this->unresolved_.push_back(
                    {.start = 0, .length = currentText.size()});
            }
            return false;
        }

        const auto end = occurrence.start + occurrence.length;
        std::erase_if(this->occurrences_, [&](const auto &existing) {
            const auto existingEnd = existing.start + existing.length;
            return existing.start < end && occurrence.start < existingEnd;
        });
        this->occurrences_.push_back(std::move(occurrence));
        std::ranges::sort(this->occurrences_, {}, &DraftEmoteOccurrence::start);
        return true;
    }

    MessageDraft snapshot(QString text) const
    {
        MessageDraft draft{
            .text = std::move(text),
            .emotes = this->occurrences_,
            .provenanceValid =
                !this->invalidInsertion_ && this->unresolved_.empty(),
        };
        for (const auto &occurrence : draft.emotes)
        {
            if (!this->matches(occurrence, draft.text))
            {
                draft.provenanceValid = false;
                break;
            }
        }
        return draft;
    }

    void clear()
    {
        ++this->revision_;
        this->occurrences_.clear();
        this->unresolved_.clear();
        this->invalidInsertion_ = false;
    }

    /// Marks a completion/picker insertion whose rendered text was known but
    /// whose provider identity or scope was ambiguous. The next document edit
    /// may correct it; submission before then fails conservatively.
    void recordUnresolvedSelection(qsizetype start, qsizetype length,
                                   QStringView currentText)
    {
        if (start < 0 || length <= 0 || start > currentText.size() ||
            length > currentText.size() - start)
        {
            this->invalidInsertion_ = true;
            return;
        }
        this->unresolved_.push_back({.start = start, .length = length});
    }

    const std::vector<DraftEmoteOccurrence> &occurrences() const noexcept
    {
        return this->occurrences_;
    }

    std::uint64_t revision() const noexcept
    {
        return this->revision_;
    }

private:
    struct Range {
        qsizetype start;
        qsizetype length;
    };

    static bool matches(const DraftEmoteOccurrence &occurrence,
                        QStringView text)
    {
        return occurrence.start >= 0 && occurrence.length > 0 &&
               occurrence.start <= text.size() &&
               occurrence.length <= text.size() - occurrence.start &&
               text.sliced(occurrence.start, occurrence.length) ==
                   occurrence.insertionText;
    }

    std::vector<DraftEmoteOccurrence> occurrences_;
    std::vector<Range> unresolved_;
    bool invalidInsertion_ = false;
    std::uint64_t revision_ = 0;
};

}  // namespace chatterino
