// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"
#include "providers/rumble/RumbleEvent.hpp"

#include <QHash>
#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <variant>

namespace chatterino::rumble {

enum class ModerationIdentityError : std::uint8_t { Empty, NotNormalized };

/// Opaque provider identity. Values are compared exactly and are never parsed
/// as numbers, display names, or account privileges.
class ModerationIdentity
{
public:
    static Expected<ModerationIdentity, ModerationIdentityError> fromProvider(
        QString value);
    const QString &value() const noexcept;
    friend bool operator==(const ModerationIdentity &,
                           const ModerationIdentity &) = default;

private:
    explicit ModerationIdentity(QString value);
    QString value_;
};

/// Typed roles are supplied only after an accepted fixture adapter has
/// identified their meaning. This layer never maps guessed wire strings.
enum class ModerationRole : std::uint8_t {
    Broadcaster,
    Moderator,
    Subscriber,
};
using ModerationRoles = QSet<ModerationRole>;

struct ModerationScope {
    std::optional<ModerationIdentity> accountId;
    ModerationIdentity channelId;
    friend bool operator==(const ModerationScope &,
                           const ModerationScope &) = default;
};

enum class ModerationCapability : std::uint8_t {
    ObserveDeletes,
    ObservePins,
    ObserveMutes,
    DeleteMessage,
    PinMessage,
    UnpinMessage,
    MuteUser,
    UnmuteUser,
    BanUser,
    UnbanUser,
};

enum class ModerationAvailability : std::uint8_t {
    Available,
    Unauthorized,
    Unsupported,
};

struct ModerationCapabilities {
    QHash<ModerationCapability, ModerationAvailability> values;
    ModerationAvailability get(ModerationCapability capability) const;
    friend bool operator==(const ModerationCapabilities &,
                           const ModerationCapabilities &) = default;
};

struct ModerationRoleSnapshot {
    ModerationScope scope;
    ModerationRoles roles;
};
struct ModerationMessageDeleted {
    ModerationScope scope;
    std::vector<ModerationIdentity> messageIds;
};
struct ModerationPinChanged {
    ModerationScope scope;
    std::optional<ModerationIdentity> messageId;
};
struct ModerationMuteChanged {
    ModerationScope scope;
    ModerationIdentity userId;
    bool muted = true;
};

using ModerationPayload =
    std::variant<ModerationRoleSnapshot, ModerationMessageDeleted,
                 ModerationPinChanged, ModerationMuteChanged>;

struct ModerationEvent {
    std::uint64_t revision = 0;
    ModerationPayload payload;
};

enum class ModerationApplyResult : std::uint8_t {
    Applied,
    Duplicate,
    Stale,
    WrongScope,
};

/// Converts only already-accepted parser DTOs. The revision is a local stream
/// arrival ordinal supplied by the connection, not a claimed wire field.
std::optional<ModerationEvent> moderationEventFromStateOperation(
    const StateOperation &operation, const ModerationScope &scope,
    std::uint64_t revision);

/// Deterministic inbound-only reducer. A reducer instance belongs to exactly
/// one account/channel pair, so state can never bleed across channel switches.
class ModerationState
{
public:
    static constexpr std::size_t DEFAULT_STATE_CAPACITY = 50000;

    explicit ModerationState(
        ModerationScope scope,
        std::size_t stateCapacity = DEFAULT_STATE_CAPACITY);

    ModerationApplyResult apply(const ModerationEvent &event);
    const ModerationScope &scope() const noexcept;
    const ModerationRoles &roles() const noexcept;
    const QSet<QString> &deletedMessageIds() const noexcept;
    const QSet<QString> &mutedUserIds() const noexcept;
    const std::optional<QString> &pinnedMessageId() const noexcept;
    bool isMuted(const ModerationIdentity &userId) const;
    ModerationCapabilities capabilities() const;

private:
    ModerationScope scope_;
    ModerationRoles roles_;
    QSet<QString> deletedMessageIds_;
    std::optional<QString> pinnedMessageId_;
    QSet<QString> mutedUserIds_;
    std::deque<QString> deletedMessageIdFifo_;
    std::deque<QString> mutedUserIdFifo_;
    QSet<QString> replayKeys_;
    std::deque<QString> replayKeyFifo_;
    std::size_t stateCapacity_;
    std::size_t replayCapacity_;
    std::optional<std::uint64_t> revision_;
};

struct ModerationSnapshot {
    ModerationScope scope;
    ModerationRoles roles;
    QSet<QString> deletedMessageIds;
    std::optional<QString> pinnedMessageId;
    QSet<QString> mutedUserIds;
    ModerationCapabilities capabilities;
};

ModerationSnapshot moderationSnapshot(const ModerationState &state);

enum class ModerationMutationError : std::uint8_t { Unsupported };

/// Outbound delete/pin/mute/ban moderation has no accepted wire contract.
/// Keeping this explicit
/// prevents UI code from guessing endpoints merely because roles were seen.
Expected<void, ModerationMutationError> requestModerationMutation(
    ModerationCapability capability);

}  // namespace chatterino::rumble
