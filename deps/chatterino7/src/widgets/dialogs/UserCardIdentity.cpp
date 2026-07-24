// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UserCardIdentity.hpp"

#include <QHash>
#include <QRegularExpression>

#include <utility>

namespace chatterino {
namespace {

QString normalizeLogin(QString name)
{
    return name.trimmed().toCaseFolded();
}

bool authorMatches(const Message &message, const QString &clickedName)
{
    return message.loginName.compare(clickedName, Qt::CaseInsensitive) == 0 ||
           message.displayName.compare(clickedName, Qt::CaseInsensitive) == 0;
}

QString metadataLabel(QString id)
{
    id = id.trimmed();
    static const QRegularExpression SAFE_ID(
        QStringLiteral("^[A-Za-z0-9]+(?:[_-][A-Za-z0-9]+)*$"));
    if (!SAFE_ID.match(id).hasMatch())
    {
        return {};
    }

    id.replace('_', ' ');
    id.replace('-', ' ');
    bool startOfWord = true;
    for (auto &character : id)
    {
        if (character.isSpace())
        {
            startOfWord = true;
        }
        else if (startOfWord)
        {
            character = character.toUpper();
            startOfWord = false;
        }
    }
    return id;
}

}  // namespace

std::size_t UserCardIdentityKeyHash::operator()(
    const UserCardIdentityKey &key) const noexcept
{
    auto seed = qHash(key.value);
    seed ^= static_cast<std::size_t>(key.provider) + 0x9e3779b9U +
            (seed << 6U) + (seed >> 2U);
    seed ^= static_cast<std::size_t>(key.kind) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
    return seed;
}

UserCardProvider userCardProvider(MessagePlatform platform) noexcept
{
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            return UserCardProvider::Twitch;
        case MessagePlatform::Kick:
            return UserCardProvider::Kick;
        case MessagePlatform::Rumble:
            return UserCardProvider::Rumble;
    }
    return UserCardProvider::Twitch;
}

UserCardIdentity userCardIdentityFromName(QString name,
                                          MessagePlatform platform)
{
    const auto normalized = normalizeLogin(name);
    return {
        .key =
            {
                .provider = userCardProvider(platform),
                .kind = UserCardIdentityKind::ProviderScopedLogin,
                .value = normalized,
            },
        .loginName = std::move(name),
        .displayName = {},
    };
}

UserCardIdentity userCardIdentityFromMessage(const Message &message,
                                             QString clickedName)
{
    auto identity =
        userCardIdentityFromName(std::move(clickedName), message.platform);
    if (!authorMatches(message, identity.loginName))
    {
        return identity;
    }

    identity.loginName = message.loginName;
    identity.displayName = message.displayName;
    identity.usernameColor = message.usernameColor;
    if (!message.userID.isEmpty())
    {
        identity.key.kind = UserCardIdentityKind::StableProviderID;
        identity.key.value = message.userID;
    }
    else
    {
        identity.key.value = normalizeLogin(identity.loginName);
    }

    if (message.platform == MessagePlatform::Rumble && message.rumble)
    {
        identity.rumbleBadgeIDs = message.rumble->badgeIDs;
        identity.rumbleRoleIDs = message.rumble->roleIDs;
    }
    return identity;
}

UserCardProfileLookup userCardProfileLookup(
    const UserCardIdentity &identity) noexcept
{
    switch (identity.key.provider)
    {
        case UserCardProvider::Twitch:
            return UserCardProfileLookup::Twitch;
        case UserCardProvider::Kick:
            return UserCardProfileLookup::Kick;
        case UserCardProvider::Rumble:
            return UserCardProfileLookup::None;
    }
    return UserCardProfileLookup::None;
}

UserCardSurfacePolicy userCardSurfacePolicy(
    const UserCardIdentity &identity) noexcept
{
    if (identity.key.provider != UserCardProvider::Rumble)
    {
        return {};
    }
    return {
        .showAvatar = false,
        .showAccountMetadata = false,
        .showTwitchProfileAction = false,
        .showCommonProfileActions = false,
        .showBlock = false,
        .showIgnoreHighlights = false,
        .showNotes = false,
        .showModeratorActions = false,
    };
}

bool userCardRequestIsCurrent(const UserCardRequestToken &request,
                              const UserCardIdentityKey &currentKey,
                              uint64_t currentGeneration) noexcept
{
    return request.generation == currentGeneration && request.key == currentKey;
}

bool messageMatchesUserCard(const UserCardIdentity &identity,
                            const Message &message)
{
    if (userCardProvider(message.platform) != identity.key.provider ||
        message.flags.has(MessageFlag::Whisper))
    {
        return false;
    }

    if (identity.key.kind == UserCardIdentityKind::StableProviderID)
    {
        if (!message.userID.isEmpty())
        {
            return message.userID == identity.key.value;
        }
        if (identity.key.provider != UserCardProvider::Twitch)
        {
            return false;
        }
    }
    else if (message.loginName.compare(identity.loginName,
                                       Qt::CaseInsensitive) == 0)
    {
        return true;
    }

    if (identity.key.provider != UserCardProvider::Twitch)
    {
        return false;
    }

    const bool isSubscription =
        message.flags.has(MessageFlag::Subscription) &&
        message.loginName.isEmpty() &&
        message.messageText.section(' ', 0, 0).compare(
            identity.loginName, Qt::CaseInsensitive) == 0;
    const bool isModAction = message.timeoutUser.compare(
                                 identity.loginName, Qt::CaseInsensitive) == 0;
    return isSubscription || isModAction;
}

QStringList rumbleUserCardMetadataLabels(const QStringList &ids)
{
    QStringList labels;
    for (const auto &id : ids)
    {
        const auto label = metadataLabel(id);
        if (!label.isEmpty() && !labels.contains(label))
        {
            labels.append(label);
        }
    }
    return labels;
}

}  // namespace chatterino
