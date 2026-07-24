// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "providers/rumble/RumbleDiagnostic.hpp"

#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace chatterino::rumble {

enum class EmoteScope : std::uint8_t { Global, Channel };

/// One provider-authoritative entry from Rumble's bounded emote.list catalog.
/// `id` is the provider group ID plus provider position. Name and image remain
/// presentation/send metadata and are snapshotted separately.
struct EmoteDefinition {
    QString id;
    QString name;
    QString imageUrl;
    EmoteScope scope = EmoteScope::Global;
    bool subscribersOnly = false;

    [[nodiscard]] QString insertionText() const;

    friend bool operator==(const EmoteDefinition &,
                           const EmoteDefinition &) = default;
};

struct EmoteCatalog {
    std::vector<EmoteDefinition> emotes;

    friend bool operator==(const EmoteCatalog &,
                           const EmoteCatalog &) = default;
};

struct EmoteCatalogParseResult {
    std::optional<EmoteCatalog> catalog;
    std::vector<Diagnostic> diagnostics;
};

/// Immutable occurrence captured while hydrating a message. Offsets and
/// lengths are QString/UTF-16 units, matching Chatterino's text boundary.
struct ResolvedEmote {
    qsizetype start = 0;
    qsizetype length = 0;
    EmoteDefinition definition;

    friend bool operator==(const ResolvedEmote &,
                           const ResolvedEmote &) = default;
};

inline constexpr qsizetype MAX_EMOTE_CATALOG_BYTES = 1024 * 1024;
inline constexpr std::size_t MAX_EMOTE_GROUPS = 64;
inline constexpr std::size_t MAX_EMOTES = 4096;
inline constexpr std::size_t MAX_EMOTES_PER_MESSAGE = 256;

EmoteCatalogParseResult parseEmoteCatalog(QByteArrayView json);
std::vector<ResolvedEmote> resolveEmoteOccurrences(
    QStringView text, const EmoteCatalog &catalog,
    std::size_t maximum = MAX_EMOTES_PER_MESSAGE);
EmotePtr makeEmote(const EmoteDefinition &definition);

}  // namespace chatterino::rumble
