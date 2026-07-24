// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleDiagnostic.hpp"
#include "providers/rumble/RumbleEmotes.hpp"

#include <QByteArrayView>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <variant>
#include <vector>

namespace chatterino::rumble {

struct UserDelta {
    QString id;
    std::optional<QString> loginName;
    std::optional<QString> displayName;
    std::optional<QColor> color;
    std::optional<QStringList> badgeIds;
    std::optional<QStringList> roleIds;
    std::optional<QString> source;
};

struct ChannelDelta {
    QString id;
    std::optional<QString> name;
};

struct BadgeDefinition {
    QString id;
    QString title;
    QString iconUrl48;
    QString iconUrl96;
};

/// Immutable badge presentation captured when a message is hydrated.
/// The provider ID is the badge identity; the title is mutable catalog
/// metadata and therefore must be snapshotted into each message.
struct ResolvedBadge {
    QString id;
    QString title;
    QString iconUrl48;
    QString iconUrl96;

    friend bool operator==(const ResolvedBadge &,
                           const ResolvedBadge &) = default;
};

struct MessageDto {
    QString id;
    QString userId;
    QString channelId;
    QString text;
    QString createdOn;
    QDateTime timestamp;
    std::uint64_t arrivalOrdinal = 0;

    std::optional<QString> loginName;
    std::optional<QString> displayName;
    std::optional<QColor> color;
    std::optional<QString> channelName;
    std::optional<QStringList> badgeIds;
    std::optional<QStringList> roleIds;
    std::optional<QString> source;
    std::vector<ResolvedBadge> resolvedBadges;
    std::vector<ResolvedEmote> resolvedEmotes;
    bool rant = false;
};

struct InitEvent {
    QHash<QString, UserDelta> users;
    QHash<QString, ChannelDelta> channels;
    QHash<QString, BadgeDefinition> badges;
    int messageLengthMax = 0;
    std::vector<MessageDto> messages;
};

struct MessagesEvent {
    QHash<QString, UserDelta> users;
    QHash<QString, ChannelDelta> channels;
    std::vector<MessageDto> messages;
};

struct DeleteMessagesEvent {
    QStringList messageIds;
};

struct DeleteNonRantMessagesEvent {
    QStringList messageIds;
    bool clearNonRant = true;
};

struct PinMessageEvent {
    MessageDto message;
};

using Event = std::variant<InitEvent, MessagesEvent, DeleteMessagesEvent,
                           DeleteNonRantMessagesEvent, PinMessageEvent>;

struct EventParseResult {
    std::optional<Event> event;
    std::vector<Diagnostic> diagnostics;
};

class EventParser
{
public:
    EventParseResult parse(QByteArrayView json);

private:
    std::uint64_t nextArrivalOrdinal_ = 0;
};

class MessageIdDeduplicator
{
public:
    static constexpr std::size_t DEFAULT_CAPACITY = 50000;

    explicit MessageIdDeduplicator(std::size_t capacity = DEFAULT_CAPACITY);

    void setStreamIdentity(QString identity);
    bool accept(const QString &messageId);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::size_t capacity_;
    QString streamIdentity_;
    std::deque<QString> fifo_;
    QSet<QString> ids_;
};

using StateOperation =
    std::variant<DeleteMessagesEvent, DeleteNonRantMessagesEvent,
                 PinMessageEvent>;

struct ProcessedEvent {
    std::vector<MessageDto> messages;
    std::vector<StateOperation> operations;
};

class EventState
{
public:
    explicit EventState(std::size_t deduplicationCapacity =
                            MessageIdDeduplicator::DEFAULT_CAPACITY);

    void replaceEmoteCatalog(QString streamIdentity, EmoteCatalog catalog);
    ProcessedEvent process(const Event &event, const QString &streamIdentity);

private:
    void applyUsers(const QHash<QString, UserDelta> &deltas);
    void applyChannels(const QHash<QString, ChannelDelta> &deltas);
    void hydrate(MessageDto &message) const;

    QString streamIdentity_;
    std::size_t catalogCapacity_;
    QHash<QString, UserDelta> users_;
    QHash<QString, ChannelDelta> channels_;
    QHash<QString, BadgeDefinition> badges_;
    QString emoteStreamIdentity_;
    EmoteCatalog emotes_;
    std::deque<QString> userFifo_;
    std::deque<QString> channelFifo_;
    MessageIdDeduplicator deduplicator_;
};

}  // namespace chatterino::rumble
