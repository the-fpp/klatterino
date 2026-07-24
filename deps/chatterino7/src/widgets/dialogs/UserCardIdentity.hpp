// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <QColor>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>

namespace chatterino {

enum class UserCardProvider : std::uint8_t {
    Twitch,
    Kick,
    Rumble,
};

enum class UserCardIdentityKind : std::uint8_t {
    StableProviderID,
    ProviderScopedLogin,
};

struct UserCardIdentityKey {
    UserCardProvider provider = UserCardProvider::Twitch;
    UserCardIdentityKind kind = UserCardIdentityKind::ProviderScopedLogin;
    QString value;

    friend bool operator==(const UserCardIdentityKey &,
                           const UserCardIdentityKey &) = default;
};

struct UserCardIdentityKeyHash {
    std::size_t operator()(const UserCardIdentityKey &key) const noexcept;
};

struct UserCardRequestToken {
    UserCardIdentityKey key;
    uint64_t generation = 0;
};

struct UserCardIdentity {
    UserCardIdentityKey key;
    QString loginName;
    QString displayName;
    QColor usernameColor;
    QStringList rumbleBadgeIDs;
    QStringList rumbleRoleIDs;
};

enum class UserCardProfileLookup : std::uint8_t {
    None,
    Twitch,
    Kick,
};

struct UserCardSurfacePolicy {
    bool showAvatar = true;
    bool showAccountMetadata = true;
    bool showTwitchProfileAction = true;
    bool showCommonProfileActions = true;
    bool showBlock = true;
    bool showIgnoreHighlights = true;
    bool showNotes = true;
    bool showModeratorActions = true;

    friend bool operator==(const UserCardSurfacePolicy &,
                           const UserCardSurfacePolicy &) = default;
};

UserCardProvider userCardProvider(MessagePlatform platform) noexcept;

UserCardIdentity userCardIdentityFromName(QString name,
                                          MessagePlatform platform);
UserCardIdentity userCardIdentityFromMessage(const Message &message,
                                             QString clickedName);

UserCardProfileLookup userCardProfileLookup(
    const UserCardIdentity &identity) noexcept;
UserCardSurfacePolicy userCardSurfacePolicy(
    const UserCardIdentity &identity) noexcept;
bool userCardRequestIsCurrent(const UserCardRequestToken &request,
                              const UserCardIdentityKey &currentKey,
                              uint64_t currentGeneration) noexcept;

bool messageMatchesUserCard(const UserCardIdentity &identity,
                            const Message &message);

QStringList rumbleUserCardMetadataLabels(const QStringList &ids);

}  // namespace chatterino
