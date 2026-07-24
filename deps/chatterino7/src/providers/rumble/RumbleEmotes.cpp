// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleEmotes.hpp"

#include "messages/Image.hpp"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace chatterino::rumble {
namespace {

using JsonValue = rapidjson::Value;

const JsonValue *member(const JsonValue &object, const char *name)
{
    if (!object.IsObject())
    {
        return nullptr;
    }
    const auto found = object.FindMember(name);
    return found == object.MemberEnd() ? nullptr : &found->value;
}

QString string(const JsonValue &value)
{
    return QString::fromUtf8(value.GetString(),
                             static_cast<qsizetype>(value.GetStringLength()));
}

void diagnose(std::vector<Diagnostic> &diagnostics, QString code, QString path)
{
    diagnostics.push_back({.code = std::move(code), .path = std::move(path)});
}

std::optional<QString> identifier(const JsonValue &value)
{
    if (value.IsString())
    {
        const auto result = string(value);
        if (!result.isEmpty() && result.size() <= 128 &&
            result == result.trimmed() &&
            std::ranges::none_of(result, [](QChar ch) {
                return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
            }))
        {
            return result;
        }
    }
    else if (value.IsUint64() && value.GetUint64() != 0)
    {
        return QString::number(value.GetUint64());
    }
    return std::nullopt;
}

bool validName(const QString &name, EmoteScope scope)
{
    static const QRegularExpression global(
        QStringLiteral("^r\\+[A-Za-z0-9]{1,61}$"));
    static const QRegularExpression channel(
        QStringLiteral("^[A-Za-z0-9]{1,64}$"));
    return (scope == EmoteScope::Global ? global : channel)
        .match(name)
        .hasMatch();
}

std::optional<QString> imageUrl(const JsonValue &value)
{
    if (!value.IsString() || value.GetStringLength() == 0 ||
        value.GetStringLength() > 2048)
    {
        return std::nullopt;
    }
    const auto raw = string(value);
    const QUrl url(raw, QUrl::StrictMode);
    static const QRegularExpression path(
        QStringLiteral("^/video/z12/[A-Za-z0-9_./-]{1,512}$"));
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().compare(QStringLiteral("1a-1791.com"),
                           Qt::CaseInsensitive) != 0 ||
        url.port(-1) != -1 || !url.userInfo().isEmpty() ||
        !path.match(url.path(QUrl::FullyDecoded)).hasMatch() ||
        !url.query().isEmpty() || url.hasFragment())
    {
        return std::nullopt;
    }
    return url.toString(QUrl::FullyEncoded);
}

bool shortcodeCharacter(QChar ch)
{
    return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
           (ch >= u'0' && ch <= u'9') || ch == u'+';
}

}  // namespace

QString EmoteDefinition::insertionText() const
{
    return u':' + this->name + u':';
}

EmoteCatalogParseResult parseEmoteCatalog(QByteArrayView json)
{
    EmoteCatalogParseResult result;
    if (json.isEmpty() || json.size() > MAX_EMOTE_CATALOG_BYTES)
    {
        diagnose(result.diagnostics, QStringLiteral("emote_catalog_limit"),
                 QStringLiteral("$"));
        return result;
    }

    rapidjson::Document document;
    document.Parse(json.data(), static_cast<std::size_t>(json.size()));
    if (document.HasParseError() || !document.IsObject())
    {
        diagnose(result.diagnostics, QStringLiteral("emote_catalog_schema"),
                 QStringLiteral("$"));
        return result;
    }
    const auto *data = member(document, "data");
    const auto *items = data ? member(*data, "items") : nullptr;
    if (items == nullptr || !items->IsArray() ||
        items->Size() > MAX_EMOTE_GROUPS)
    {
        diagnose(result.diagnostics, QStringLiteral("emote_catalog_schema"),
                 QStringLiteral("data.items"));
        return result;
    }

    EmoteCatalog catalog;
    QSet<QString> identities;
    QSet<QString> shortcodes;
    for (rapidjson::SizeType groupIndex = 0; groupIndex < items->Size();
         ++groupIndex)
    {
        const auto &group = (*items)[groupIndex];
        if (!group.IsObject())
        {
            diagnose(result.diagnostics, QStringLiteral("invalid_emote_group"),
                     QStringLiteral("data.items[]"));
            continue;
        }
        const auto *groupIdValue = member(group, "id");
        const auto *channelIdValue = member(group, "channel_id");
        const auto *emotes = member(group, "emotes");
        const auto groupId =
            groupIdValue ? identifier(*groupIdValue) : std::optional<QString>{};
        if (!groupId || channelIdValue == nullptr || emotes == nullptr ||
            !emotes->IsArray())
        {
            diagnose(result.diagnostics, QStringLiteral("invalid_emote_group"),
                     QStringLiteral("data.items[]"));
            continue;
        }
        const auto scope =
            channelIdValue->IsNull() ? EmoteScope::Global : EmoteScope::Channel;
        if (scope == EmoteScope::Channel && !identifier(*channelIdValue))
        {
            diagnose(result.diagnostics, QStringLiteral("invalid_emote_group"),
                     QStringLiteral("data.items[].channel_id"));
            continue;
        }

        for (rapidjson::SizeType emoteIndex = 0; emoteIndex < emotes->Size();
             ++emoteIndex)
        {
            if (catalog.emotes.size() >= MAX_EMOTES)
            {
                diagnose(result.diagnostics,
                         QStringLiteral("emote_catalog_limit"),
                         QStringLiteral("data.items[].emotes"));
                result.catalog = std::move(catalog);
                return result;
            }
            const auto &emote = (*emotes)[emoteIndex];
            const auto *nameValue = member(emote, "name");
            const auto *fileValue = member(emote, "file");
            const auto *positionValue = member(emote, "position");
            const auto *subscriberValue = member(emote, "is_subs_only");
            if (!emote.IsObject() || nameValue == nullptr ||
                !nameValue->IsString() || fileValue == nullptr ||
                positionValue == nullptr || !positionValue->IsUint() ||
                subscriberValue == nullptr || !subscriberValue->IsBool())
            {
                diagnose(result.diagnostics,
                         QStringLiteral("invalid_emote_definition"),
                         QStringLiteral("data.items[].emotes[]"));
                continue;
            }
            const auto name = string(*nameValue);
            const auto file = imageUrl(*fileValue);
            const auto position = positionValue->GetUint();
            const auto identity = *groupId + u':' + QString::number(position);
            const auto insertion = u':' + name + u':';
            if (!validName(name, scope) || !file ||
                identities.contains(identity) || shortcodes.contains(insertion))
            {
                diagnose(result.diagnostics,
                         QStringLiteral("invalid_emote_definition"),
                         QStringLiteral("data.items[].emotes[]"));
                continue;
            }
            identities.insert(identity);
            shortcodes.insert(insertion);
            catalog.emotes.push_back({
                .id = identity,
                .name = name,
                .imageUrl = *file,
                .scope = scope,
                .subscribersOnly = subscriberValue->GetBool(),
            });
        }
    }
    result.catalog = std::move(catalog);
    return result;
}

std::vector<ResolvedEmote> resolveEmoteOccurrences(QStringView text,
                                                   const EmoteCatalog &catalog,
                                                   std::size_t maximum)
{
    QHash<QString, const EmoteDefinition *> definitions;
    definitions.reserve(static_cast<qsizetype>(catalog.emotes.size()));
    for (const auto &definition : catalog.emotes)
    {
        definitions.insert(definition.insertionText(), &definition);
    }

    std::vector<ResolvedEmote> result;
    result.reserve(std::min(maximum, catalog.emotes.size()));
    qsizetype cursor = 0;
    while (cursor < text.size() && result.size() < maximum)
    {
        const auto start = text.indexOf(u':', cursor);
        if (start < 0 || start + 2 >= text.size())
        {
            break;
        }
        auto end = start + 1;
        while (end < text.size() && end - start <= 66 &&
               shortcodeCharacter(text.at(end)))
        {
            ++end;
        }
        if (end < text.size() && text.at(end) == u':' && end > start + 1)
        {
            const auto token = text.mid(start, end - start + 1).toString();
            const auto found = definitions.constFind(token);
            if (found != definitions.cend())
            {
                result.push_back({
                    .start = start,
                    .length = end - start + 1,
                    .definition = **found,
                });
            }
            cursor = end + 1;
            continue;
        }
        cursor = start + 1;
    }
    return result;
}

EmotePtr makeEmote(const EmoteDefinition &definition)
{
    return std::make_shared<const Emote>(Emote{
        .name = {definition.name},
        .images = ImageSet(Image::fromAutoscaledUrl({definition.imageUrl}, 28)),
        .tooltip = {definition.name.toHtmlEscaped() +
                    QStringLiteral("<br>Rumble Emote")},
        .id = {definition.id},
    });
}

}  // namespace chatterino::rumble
