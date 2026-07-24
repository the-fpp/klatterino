// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "messages/MessageDraft.hpp"
#include "providers/emoji/Emojis.hpp"

#include <utility>

namespace chatterino {

inline void appendDraftEmoteCapabilities(
    std::vector<DraftEmoteCapability> &out, const EmoteMap &emotes,
    const QString &provider, const DraftEmoteAvailability &availability)
{
    for (const auto &[name, emote] : emotes)
    {
        if (!emote || emote->id.string.isEmpty() || name.string.isEmpty())
        {
            // Both stable identity and exact provider-recognized text are
            // required for a submit-time capability.
            continue;
        }
        out.push_back({
            .identity = {.provider = provider, .id = emote->id},
            .insertionText = name.string,
            .availability = availability,
        });
    }
}

inline void appendDraftEmojiCapabilities(
    std::vector<DraftEmoteCapability> &out,
    const std::vector<EmojiPtr> &emojis)
{
    for (const auto &emoji : emojis)
    {
        if (!emoji || emoji->unifiedCode.isEmpty())
        {
            continue;
        }
        for (const auto &shortCode : emoji->shortCodes)
        {
            if (shortCode.isEmpty())
            {
                continue;
            }
            out.push_back({
                .identity = {
                    .provider = QStringLiteral("emoji"),
                    .id = EmoteId{emoji->unifiedCode},
                },
                .insertionText = QStringLiteral(":%1:").arg(shortCode),
                .availability = {},
            });
        }
    }
}

}  // namespace chatterino
