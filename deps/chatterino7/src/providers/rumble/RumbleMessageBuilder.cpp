// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleMessageBuilder.hpp"

#include "common/LinkParser.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"

#include <utility>

namespace chatterino::rumble {
namespace {

constexpr qreal BADGE_DISPLAY_SIZE = 18.0;

MessageElementFlag badgeFlag(QStringView id)
{
    if (id == u"admin")
    {
        return MessageElementFlag::BadgeGlobalAuthority;
    }
    if (id == u"moderator" || id == u"broadcaster")
    {
        return MessageElementFlag::BadgeChannelAuthority;
    }
    if (id == u"recurring_subscription" || id == u"locals_supporter" ||
        id == u"subscriber")
    {
        return MessageElementFlag::BadgeSubscription;
    }
    return MessageElementFlag::BadgeVanity;
}

EmotePtr badgeEmote(const ResolvedBadge &badge)
{
    const auto firstUrl =
        badge.iconUrl48.isEmpty() ? badge.iconUrl96 : badge.iconUrl48;
    if (firstUrl.isEmpty())
    {
        return nullptr;
    }

    const auto firstSize = badge.iconUrl48.isEmpty() ? 96 : 48;
    auto first = Image::fromUrl({firstUrl}, BADGE_DISPLAY_SIZE / firstSize,
                                {firstSize, firstSize});
    auto second = getEmptyImagePtr();
    if (!badge.iconUrl48.isEmpty() && !badge.iconUrl96.isEmpty())
    {
        second = Image::fromUrl({badge.iconUrl96}, BADGE_DISPLAY_SIZE / 96.0,
                                {96, 96});
    }

    return std::make_shared<const Emote>(Emote{
        .name = {badge.title},
        .images = ImageSet{first, second},
        .tooltip = {badge.title.toHtmlEscaped()},
        .id = {badge.id},
    });
}

MessageElement *appendTextOrLink(MessageBuilder &builder, QStringView source)
{
    const auto parsedLink = linkparser::parse(source);
    if (!parsedLink)
    {
        return builder.emplace<TextElement>(source.toString(),
                                            MessageElementFlag::Text,
                                            MessageColor(MessageColor::Text));
    }

    QString lowercaseLink;
    const auto originalLink = parsedLink->link.toString();
    QString fullUrl;
    if (parsedLink->protocol.isNull())
    {
        fullUrl = QStringLiteral("http://") + originalLink;
    }
    else
    {
        lowercaseLink += parsedLink->protocol;
        fullUrl = originalLink;
    }
    lowercaseLink += parsedLink->host.toString().toLower();
    lowercaseLink += parsedLink->rest;

    if (parsedLink->hasPrefix(source))
    {
        builder
            .emplace<TextElement>(parsedLink->prefix(source).toString(),
                                  MessageElementFlag::Text,
                                  MessageColor(MessageColor::Text))
            ->setTrailingSpace(false);
    }

    auto *link = builder.emplace<LinkElement>(
        LinkElement::Parsed{
            .lowercase = std::move(lowercaseLink),
            .original = originalLink,
        },
        fullUrl, MessageElementFlag::Text, MessageColor(MessageColor::Link));
    if (parsedLink->hasSuffix(source))
    {
        link->setTrailingSpace(false);
        builder.emplace<TextElement>(parsedLink->suffix(source).toString(),
                                     MessageElementFlag::Text,
                                     MessageColor(MessageColor::Text));
    }
    return builder->elements.back().get();
}

void appendText(MessageBuilder &builder, QStringView source)
{
    const auto endsWithSpace = !source.isEmpty() && source.back().isSpace();
    MessageElement *last = nullptr;
    for (const auto &word : source.split(u' ', Qt::SkipEmptyParts))
    {
        last = appendTextOrLink(builder, word);
    }
    if (last != nullptr && !endsWithSpace)
    {
        last->setTrailingSpace(false);
    }
}

}  // namespace

MessagePtrMut buildMessage(const MessageDto &dto)
{
    MessageBuilder builder;

    const auto loginName = dto.loginName.value_or(dto.userId).toLower();
    const auto displayName = dto.displayName.value_or(loginName);
    const auto usernameColor = dto.color.value_or(QColor(153, 153, 153));
    const auto channelName = dto.channelName.value_or(dto.channelId);

    builder->id = dto.id;
    builder->userID = dto.userId;
    builder->loginName = loginName;
    builder->displayName = displayName;
    builder->channelName = channelName;
    builder->usernameColor = usernameColor;
    builder->serverReceivedTime = dto.timestamp;
    builder->parseTime = dto.timestamp.time();
    builder->platform = MessagePlatform::Rumble;
    builder->messageText = dto.text;
    builder->searchText =
        loginName + ' ' + displayName + QStringLiteral(": ") + dto.text;
    builder->rumble = RumbleMessageMetadata{
        .channelID = dto.channelId,
        .badgeIDs = dto.badgeIds.value_or(QStringList{}),
        .roleIDs = dto.roleIds.value_or(QStringList{}),
        .source = dto.source.value_or(QString{}),
    };

    builder.emplace<TimestampElement>(dto.timestamp.toLocalTime().time());
    for (const auto &badge : dto.resolvedBadges)
    {
        if (auto emote = badgeEmote(badge))
        {
            builder.emplace<BadgeElement>(emote, badgeFlag(badge.id));
        }
    }
    builder
        .emplace<TextElement>(
            displayName + u':',
            MessageElementFlags{MessageElementFlag::Username,
                                MessageElementFlag::RumbleUsername},
            usernameColor, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, loginName});

    if (dto.resolvedEmotes.empty())
    {
        for (const auto &word : dto.text.split(' ', Qt::SkipEmptyParts))
        {
            appendTextOrLink(builder, word);
        }
    }
    else
    {
        qsizetype cursor = 0;
        for (const auto &occurrence : dto.resolvedEmotes)
        {
            if (occurrence.start < cursor || occurrence.length <= 0 ||
                occurrence.start + occurrence.length > dto.text.size())
            {
                continue;
            }
            appendText(builder, QStringView(dto.text).mid(
                                    cursor, occurrence.start - cursor));
            auto *element = builder.emplace<EmoteElement>(
                makeEmote(occurrence.definition), MessageElementFlag::Emote);
            const auto next = occurrence.start + occurrence.length;
            element->setTrailingSpace(next < dto.text.size() &&
                                      dto.text.at(next).isSpace());
            cursor = next;
        }
        appendText(builder, QStringView(dto.text).mid(cursor));
    }

    return builder.release();
}

}  // namespace chatterino::rumble
