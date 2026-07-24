// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleEvent.hpp"

#include <QRegularExpression>
#include <QUrl>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace chatterino::rumble {
namespace {

using JsonValue = rapidjson::Value;

QString fromJsonString(const JsonValue &value)
{
    return QString::fromUtf8(value.GetString(),
                             static_cast<qsizetype>(value.GetStringLength()));
}

const JsonValue *member(const JsonValue &object, const char *name)
{
    if (!object.IsObject())
    {
        return nullptr;
    }
    const auto found = object.FindMember(name);
    return found == object.MemberEnd() ? nullptr : &found->value;
}

void diagnose(std::vector<Diagnostic> &diagnostics, QString code, QString path)
{
    diagnostics.push_back({.code = std::move(code), .path = std::move(path)});
}

std::optional<QString> requiredString(const JsonValue &object, const char *name,
                                      const QString &path,
                                      std::vector<Diagnostic> &diagnostics)
{
    const auto *value = member(object, name);
    if (value == nullptr)
    {
        diagnose(diagnostics, QStringLiteral("missing_required_field"), path);
        return std::nullopt;
    }
    if (!value->IsString())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"), path);
        return std::nullopt;
    }
    return fromJsonString(*value);
}

std::optional<QString> optionalString(const JsonValue &object, const char *name,
                                      const QString &path,
                                      std::vector<Diagnostic> &diagnostics)
{
    const auto *value = member(object, name);
    if (value == nullptr || value->IsNull())
    {
        return std::nullopt;
    }
    if (!value->IsString())
    {
        diagnose(diagnostics, QStringLiteral("invalid_optional_field"), path);
        return std::nullopt;
    }
    return fromJsonString(*value);
}

std::optional<QString> badgeIconUrl(const JsonValue &definition,
                                    const char *size, const QString &path,
                                    std::vector<Diagnostic> &diagnostics)
{
    const auto *icons = member(definition, "icons");
    if (icons == nullptr || icons->IsNull())
    {
        return std::nullopt;
    }
    if (!icons->IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_optional_field"),
                 QStringLiteral("data.config.badges[*].icons"));
        return std::nullopt;
    }

    auto icon = optionalString(*icons, size, path, diagnostics);
    if (!icon || icon->isEmpty())
    {
        return std::nullopt;
    }

    QUrl url(*icon);
    if (url.isRelative() && icon->startsWith(u'/'))
    {
        url = QUrl(QStringLiteral("https://rumble.com")).resolved(url);
    }

    const auto host = url.host().toLower();
    const bool allowedHost = host == QStringLiteral("rumble.com") ||
                             host == QStringLiteral("www.rumble.com") ||
                             host == QStringLiteral("static.rumble.com");
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        !allowedHost || !url.path().startsWith(QStringLiteral("/i/badges/")) ||
        !url.query().isEmpty() || url.hasFragment())
    {
        diagnose(diagnostics, QStringLiteral("invalid_badge_icon"), path);
        return std::nullopt;
    }

    return url.toString(QUrl::FullyEncoded);
}

std::optional<QStringList> optionalStringList(
    const JsonValue &object, const char *name, const QString &path,
    std::vector<Diagnostic> &diagnostics)
{
    const auto *value = member(object, name);
    if (value == nullptr || value->IsNull())
    {
        return std::nullopt;
    }
    if (!value->IsArray())
    {
        diagnose(diagnostics, QStringLiteral("invalid_optional_field"), path);
        return std::nullopt;
    }

    QStringList values;
    for (rapidjson::SizeType index = 0; index < value->Size(); ++index)
    {
        const auto &item = (*value)[index];
        if (!item.IsString())
        {
            diagnose(diagnostics, QStringLiteral("invalid_optional_field"),
                     path + '[' + QString::number(index) + ']');
            continue;
        }
        values.push_back(fromJsonString(item));
    }
    return values;
}

std::optional<QString> normalizeMessageId(const JsonValue &value,
                                          const QString &path,
                                          std::vector<Diagnostic> &diagnostics)
{
    if (value.IsString())
    {
        auto id = fromJsonString(value);
        const bool normalized =
            !id.isEmpty() && id == id.trimmed() &&
            std::ranges::none_of(id, [](QChar ch) {
                return ch.unicode() == 0 ||
                       ch.category() == QChar::Other_Control;
            });
        if (normalized)
        {
            return id;
        }
    }
    else if (value.IsUint64())
    {
        return QString::number(value.GetUint64());
    }

    diagnose(diagnostics, QStringLiteral("invalid_message_id"), path);
    return std::nullopt;
}

std::optional<QString> normalizeIdentifier(const JsonValue &value,
                                           const QString &path,
                                           std::vector<Diagnostic> &diagnostics)
{
    if (value.IsString())
    {
        auto id = fromJsonString(value);
        const bool normalized =
            !id.isEmpty() && id == id.trimmed() &&
            std::ranges::none_of(id, [](QChar ch) {
                return ch.unicode() == 0 ||
                       ch.category() == QChar::Other_Control;
            });
        if (normalized)
        {
            return id;
        }
    }
    else if (value.IsUint64())
    {
        return QString::number(value.GetUint64());
    }

    diagnose(diagnostics, QStringLiteral("invalid_identifier"), path);
    return std::nullopt;
}

std::optional<QString> requiredIdentifier(const JsonValue &object,
                                          const char *name, const QString &path,
                                          std::vector<Diagnostic> &diagnostics)
{
    const auto *value = member(object, name);
    if (value == nullptr)
    {
        diagnose(diagnostics, QStringLiteral("missing_required_field"), path);
        return std::nullopt;
    }
    return normalizeIdentifier(*value, path, diagnostics);
}

std::optional<QString> optionalIdentifier(const JsonValue &object,
                                          const char *name, const QString &path,
                                          std::vector<Diagnostic> &diagnostics)
{
    const auto *value = member(object, name);
    if (value == nullptr || value->IsNull())
    {
        return std::nullopt;
    }
    return normalizeIdentifier(*value, path, diagnostics);
}

std::optional<QDateTime> parseTimestamp(const QString &source,
                                        const QString &path,
                                        std::vector<Diagnostic> &diagnostics)
{
    static const QRegularExpression OFFSET_SUFFIX(
        QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})$"));
    if (!OFFSET_SUFFIX.match(source).hasMatch())
    {
        diagnose(diagnostics, QStringLiteral("invalid_timestamp"), path);
        return std::nullopt;
    }

    auto timestamp = QDateTime::fromString(source, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(source, Qt::ISODate);
    }
    if (!timestamp.isValid())
    {
        diagnose(diagnostics, QStringLiteral("invalid_timestamp"), path);
        return std::nullopt;
    }
    return timestamp.toUTC();
}

void parsePresentation(const JsonValue &object, const QString &path,
                       MessageDto &message,
                       std::vector<Diagnostic> &diagnostics)
{
    message.loginName = optionalString(
        object, "username", path + QStringLiteral(".username"), diagnostics);
    message.displayName =
        optionalString(object, "display_name",
                       path + QStringLiteral(".display_name"), diagnostics);
    message.badgeIds = optionalStringList(
        object, "badges", path + QStringLiteral(".badges"), diagnostics);
    message.roleIds = optionalStringList(
        object, "roles", path + QStringLiteral(".roles"), diagnostics);
    message.source = optionalString(
        object, "source", path + QStringLiteral(".source"), diagnostics);

    if (const auto color = optionalString(
            object, "color", path + QStringLiteral(".color"), diagnostics))
    {
        const QColor parsed(*color);
        if (parsed.isValid())
        {
            message.color = parsed;
        }
        else
        {
            diagnose(diagnostics, QStringLiteral("invalid_color"),
                     path + QStringLiteral(".color"));
        }
    }

    if (const auto *rant = member(object, "is_rant");
        rant != nullptr && !rant->IsNull())
    {
        if (rant->IsBool())
        {
            message.rant = rant->GetBool();
        }
        else
        {
            diagnose(diagnostics, QStringLiteral("invalid_optional_field"),
                     path + QStringLiteral(".is_rant"));
        }
    }

    if (const auto *rant = member(object, "rant");
        rant != nullptr && !rant->IsNull())
    {
        if (rant->IsObject())
        {
            message.rant = true;
        }
        else
        {
            diagnose(diagnostics, QStringLiteral("invalid_optional_field"),
                     path + QStringLiteral(".rant"));
        }
    }

    if (const auto type = optionalString(
            object, "type", path + QStringLiteral(".type"), diagnostics))
    {
        if (*type == QStringLiteral("rant"))
        {
            message.rant = true;
        }
    }
}

std::optional<MessageDto> parseMessage(const JsonValue &value,
                                       const QString &path,
                                       std::vector<Diagnostic> &diagnostics,
                                       std::uint64_t &nextOrdinal)
{
    if (!value.IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_record"), path);
        return std::nullopt;
    }

    const auto *idValue = member(value, "id");
    std::optional<QString> id;
    if (idValue == nullptr)
    {
        diagnose(diagnostics, QStringLiteral("missing_required_field"),
                 path + QStringLiteral(".id"));
    }
    else
    {
        id = normalizeMessageId(*idValue, path + QStringLiteral(".id"),
                                diagnostics);
    }

    auto userId = requiredIdentifier(
        value, "user_id", path + QStringLiteral(".user_id"), diagnostics);
    auto channelId = optionalIdentifier(
        value, "channel_id", path + QStringLiteral(".channel_id"), diagnostics);
    auto text = requiredString(value, "text", path + QStringLiteral(".text"),
                               diagnostics);
    // The current browser client consumes `time`. Retain `created_on` as a
    // compatibility fallback for older preserved observations.
    const auto *timeValue = member(value, "time");
    const auto *timeName = timeValue == nullptr ? "created_on" : "time";
    const auto timePath =
        path + (timeValue == nullptr ? QStringLiteral(".created_on")
                                     : QStringLiteral(".time"));
    auto createdOn = requiredString(value, timeName, timePath, diagnostics);
    std::optional<QDateTime> timestamp;
    if (createdOn)
    {
        timestamp = parseTimestamp(*createdOn, timePath, diagnostics);
    }

    if (!id || !userId || userId->isEmpty() || !text || !createdOn ||
        !timestamp)
    {
        if (userId && userId->isEmpty())
        {
            diagnose(diagnostics, QStringLiteral("missing_required_field"),
                     path + QStringLiteral(".user_id"));
        }
        return std::nullopt;
    }

    MessageDto parsed{
        .id = std::move(*id),
        .userId = std::move(*userId),
        .channelId = channelId.value_or(QString{}),
        .text = std::move(*text),
        .createdOn = std::move(*createdOn),
        .timestamp = std::move(*timestamp),
        .arrivalOrdinal = nextOrdinal++,
    };
    parsePresentation(value, path, parsed, diagnostics);
    return parsed;
}

std::vector<MessageDto> parseMessages(const JsonValue &data, const char *name,
                                      const QString &path,
                                      std::vector<Diagnostic> &diagnostics,
                                      std::uint64_t &nextOrdinal)
{
    std::vector<MessageDto> messages;
    const auto *value = member(data, name);
    if (value == nullptr)
    {
        return messages;
    }
    if (!value->IsArray())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"), path);
        return messages;
    }

    messages.reserve(value->Size());
    for (rapidjson::SizeType index = 0; index < value->Size(); ++index)
    {
        if (auto message = parseMessage(
                (*value)[index], path + '[' + QString::number(index) + ']',
                diagnostics, nextOrdinal))
        {
            messages.push_back(std::move(*message));
        }
    }
    return messages;
}

QHash<QString, UserDelta> parseUsers(const JsonValue &data,
                                     std::vector<Diagnostic> &diagnostics)
{
    QHash<QString, UserDelta> users;
    const auto *value = member(data, "users");
    if (value == nullptr)
    {
        return users;
    }
    if (!value->IsArray() && !value->IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data.users"));
        return users;
    }

    const auto parseUser = [&](const JsonValue &record,
                               std::optional<QString> id) {
        const auto path = QStringLiteral("data.users[*]");
        if (!record.IsObject())
        {
            diagnose(diagnostics, QStringLiteral("invalid_user_delta"), path);
            return;
        }
        if (!id)
        {
            id = requiredIdentifier(record, "id", path + QStringLiteral(".id"),
                                    diagnostics);
        }
        if (!id)
        {
            return;
        }

        UserDelta delta{.id = *id};
        delta.loginName =
            optionalString(record, "username",
                           path + QStringLiteral(".username"), diagnostics);
        delta.displayName =
            optionalString(record, "display_name",
                           path + QStringLiteral(".display_name"), diagnostics);
        delta.badgeIds = optionalStringList(
            record, "badges", path + QStringLiteral(".badges"), diagnostics);
        delta.roleIds = optionalStringList(
            record, "roles", path + QStringLiteral(".roles"), diagnostics);
        delta.source = optionalString(
            record, "source", path + QStringLiteral(".source"), diagnostics);
        if (const auto color = optionalString(
                record, "color", path + QStringLiteral(".color"), diagnostics))
        {
            const QColor parsed(*color);
            if (parsed.isValid())
            {
                delta.color = parsed;
            }
            else
            {
                diagnose(diagnostics, QStringLiteral("invalid_color"),
                         path + QStringLiteral(".color"));
            }
        }
        users.insert(*id, std::move(delta));
    };

    if (value->IsArray())
    {
        for (const auto &record : value->GetArray())
        {
            parseUser(record, std::nullopt);
        }
    }
    else
    {
        for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it)
        {
            const auto id = QString::fromUtf8(
                it->name.GetString(),
                static_cast<qsizetype>(it->name.GetStringLength()));
            parseUser(it->value,
                      id.isEmpty() ? std::nullopt : std::optional<QString>{id});
        }
    }
    return users;
}

QHash<QString, ChannelDelta> parseChannels(const JsonValue &data,
                                           std::vector<Diagnostic> &diagnostics)
{
    QHash<QString, ChannelDelta> channels;
    const auto *value = member(data, "channels");
    if (value == nullptr)
    {
        return channels;
    }
    if (!value->IsArray() && !value->IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data.channels"));
        return channels;
    }

    const auto parseChannel = [&](const JsonValue &record,
                                  std::optional<QString> id) {
        const auto path = QStringLiteral("data.channels[*]");
        if (!record.IsObject())
        {
            diagnose(diagnostics, QStringLiteral("invalid_channel_delta"),
                     path);
            return;
        }
        if (!id)
        {
            id = requiredIdentifier(record, "id", path + QStringLiteral(".id"),
                                    diagnostics);
        }
        if (!id)
        {
            return;
        }

        auto name =
            optionalString(record, "username",
                           path + QStringLiteral(".username"), diagnostics);
        if (!name)
        {
            name = optionalString(record, "title",
                                  path + QStringLiteral(".title"), diagnostics);
        }
        channels.insert(*id, ChannelDelta{.id = *id, .name = std::move(name)});
    };

    if (value->IsArray())
    {
        for (const auto &record : value->GetArray())
        {
            parseChannel(record, std::nullopt);
        }
    }
    else
    {
        for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it)
        {
            const auto id = QString::fromUtf8(
                it->name.GetString(),
                static_cast<qsizetype>(it->name.GetStringLength()));
            parseChannel(it->value, id.isEmpty() ? std::nullopt
                                                 : std::optional<QString>{id});
        }
    }
    return channels;
}

QHash<QString, BadgeDefinition> parseBadges(
    const JsonValue &data, std::vector<Diagnostic> &diagnostics)
{
    QHash<QString, BadgeDefinition> badges;
    const auto *config = member(data, "config");
    if (config == nullptr)
    {
        return badges;
    }
    if (!config->IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data.config"));
        return badges;
    }
    const auto *value = member(*config, "badges");
    if (value == nullptr)
    {
        return badges;
    }
    if (!value->IsObject())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data.config.badges"));
        return badges;
    }

    for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it)
    {
        const auto id = QString::fromUtf8(
            it->name.GetString(),
            static_cast<qsizetype>(it->name.GetStringLength()));
        if (id.isEmpty() || !it->value.IsObject())
        {
            diagnose(diagnostics, QStringLiteral("invalid_badge_definition"),
                     QStringLiteral("data.config.badges[*]"));
            continue;
        }
        auto title = optionalString(
            it->value, "title", QStringLiteral("data.config.badges[*].title"),
            diagnostics);
        if (!title)
        {
            if (const auto *label = member(it->value, "label");
                label != nullptr && !label->IsNull())
            {
                if (label->IsObject())
                {
                    title = optionalString(
                        *label, "en",
                        QStringLiteral("data.config.badges[*].label.en"),
                        diagnostics);
                }
                else
                {
                    diagnose(diagnostics,
                             QStringLiteral("invalid_optional_field"),
                             QStringLiteral("data.config.badges[*].label"));
                }
            }
        }
        auto displayTitle = title && !title->isEmpty() ? std::move(*title) : id;
        auto iconUrl48 = badgeIconUrl(
            it->value, "48", QStringLiteral("data.config.badges[*].icons.48"),
            diagnostics);
        auto iconUrl96 = badgeIconUrl(
            it->value, "96", QStringLiteral("data.config.badges[*].icons.96"),
            diagnostics);
        badges.insert(id, {
                              .id = id,
                              .title = std::move(displayTitle),
                              .iconUrl48 = iconUrl48.value_or(QString{}),
                              .iconUrl96 = iconUrl96.value_or(QString{}),
                          });
    }
    return badges;
}

int parseMessageLengthMax(const JsonValue &data,
                          std::vector<Diagnostic> &diagnostics)
{
    const auto *config = member(data, "config");
    if (config == nullptr || !config->IsObject())
    {
        return 0;
    }
    const auto *value = member(*config, "message_length_max");
    if (value == nullptr)
    {
        return 0;
    }
    if (!value->IsInt() || value->GetInt() < 0)
    {
        diagnose(diagnostics, QStringLiteral("invalid_optional_field"),
                 QStringLiteral("data.config.message_length_max"));
        return 0;
    }
    return value->GetInt();
}

QStringList parseMessageIds(const JsonValue &data, bool required,
                            std::vector<Diagnostic> &diagnostics)
{
    QStringList ids;
    const auto *value = member(data, "message_ids");
    if (value == nullptr)
    {
        if (required)
        {
            diagnose(diagnostics, QStringLiteral("missing_required_field"),
                     QStringLiteral("data.message_ids"));
        }
        return ids;
    }
    if (!value->IsArray())
    {
        diagnose(diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data.message_ids"));
        return ids;
    }

    for (rapidjson::SizeType index = 0; index < value->Size(); ++index)
    {
        if (auto id = normalizeMessageId((*value)[index],
                                         QStringLiteral("data.message_ids[") +
                                             QString::number(index) + ']',
                                         diagnostics))
        {
            ids.push_back(std::move(*id));
        }
    }
    return ids;
}

template <typename T>
void mergeOptional(std::optional<T> &target, const std::optional<T> &source)
{
    if (source)
    {
        target = source;
    }
}

}  // namespace

EventParseResult EventParser::parse(QByteArrayView json)
{
    EventParseResult result;
    rapidjson::Document document;
    document.Parse<rapidjson::kParseValidateEncodingFlag>(
        json.data(), static_cast<std::size_t>(json.size()));
    if (document.HasParseError() || !document.IsObject())
    {
        diagnose(result.diagnostics, QStringLiteral("malformed_json"),
                 QStringLiteral("$"));
        return result;
    }

    const auto *typeValue = member(document, "type");
    if (typeValue == nullptr || !typeValue->IsString())
    {
        diagnose(result.diagnostics, QStringLiteral("missing_event_type"),
                 QStringLiteral("type"));
        return result;
    }
    const auto type = fromJsonString(*typeValue);

    if (type != QStringLiteral("init") && type != QStringLiteral("messages") &&
        type != QStringLiteral("delete_messages") &&
        type != QStringLiteral("delete_non_rant_messages") &&
        type != QStringLiteral("pin_message"))
    {
        diagnose(result.diagnostics, QStringLiteral("unknown_event_type"),
                 QStringLiteral("type"));
        return result;
    }

    const auto *data = member(document, "data");
    if (data == nullptr || !data->IsObject())
    {
        diagnose(result.diagnostics, QStringLiteral("invalid_field_type"),
                 QStringLiteral("data"));
        return result;
    }

    if (type == QStringLiteral("init"))
    {
        result.event = InitEvent{
            .users = parseUsers(*data, result.diagnostics),
            .channels = parseChannels(*data, result.diagnostics),
            .badges = parseBadges(*data, result.diagnostics),
            .messageLengthMax =
                parseMessageLengthMax(*data, result.diagnostics),
            .messages = parseMessages(
                *data, "messages", QStringLiteral("data.messages"),
                result.diagnostics, this->nextArrivalOrdinal_),
        };
    }
    else if (type == QStringLiteral("messages"))
    {
        result.event = MessagesEvent{
            .users = parseUsers(*data, result.diagnostics),
            .channels = parseChannels(*data, result.diagnostics),
            .messages = parseMessages(
                *data, "messages", QStringLiteral("data.messages"),
                result.diagnostics, this->nextArrivalOrdinal_),
        };
    }
    else if (type == QStringLiteral("delete_messages"))
    {
        result.event = DeleteMessagesEvent{
            .messageIds = parseMessageIds(*data, true, result.diagnostics),
        };
    }
    else if (type == QStringLiteral("delete_non_rant_messages"))
    {
        bool clearNonRant = true;
        if (const auto *clear = member(*data, "clear"); clear != nullptr)
        {
            if (clear->IsBool())
            {
                clearNonRant = clear->GetBool();
            }
            else
            {
                diagnose(result.diagnostics,
                         QStringLiteral("invalid_optional_field"),
                         QStringLiteral("data.clear"));
            }
        }
        result.event = DeleteNonRantMessagesEvent{
            .messageIds = parseMessageIds(*data, false, result.diagnostics),
            .clearNonRant = clearNonRant,
        };
    }
    else
    {
        const auto *message = member(*data, "message");
        if (message == nullptr)
        {
            diagnose(result.diagnostics,
                     QStringLiteral("missing_required_field"),
                     QStringLiteral("data.message"));
            return result;
        }
        auto parsed =
            parseMessage(*message, QStringLiteral("data.message"),
                         result.diagnostics, this->nextArrivalOrdinal_);
        if (parsed)
        {
            result.event = PinMessageEvent{.message = std::move(*parsed)};
        }
    }

    return result;
}

MessageIdDeduplicator::MessageIdDeduplicator(std::size_t capacity)
    : capacity_(capacity)
{
    if (this->capacity_ == 0)
    {
        this->capacity_ = 1;
    }
}

void MessageIdDeduplicator::setStreamIdentity(QString identity)
{
    if (identity == this->streamIdentity_)
    {
        return;
    }
    this->streamIdentity_ = std::move(identity);
    this->fifo_.clear();
    this->ids_.clear();
}

bool MessageIdDeduplicator::accept(const QString &messageId)
{
    if (messageId.isEmpty() || this->ids_.contains(messageId))
    {
        return false;
    }

    if (this->fifo_.size() == this->capacity_)
    {
        this->ids_.remove(this->fifo_.front());
        this->fifo_.pop_front();
    }
    this->fifo_.push_back(messageId);
    this->ids_.insert(messageId);
    return true;
}

std::size_t MessageIdDeduplicator::size() const noexcept
{
    return this->fifo_.size();
}

EventState::EventState(std::size_t deduplicationCapacity)
    : catalogCapacity_(std::max<std::size_t>(1, deduplicationCapacity))
    , deduplicator_(deduplicationCapacity)
{
}

void EventState::replaceEmoteCatalog(QString streamIdentity,
                                     EmoteCatalog catalog)
{
    this->emoteStreamIdentity_ = std::move(streamIdentity);
    this->emotes_ = std::move(catalog);
}

ProcessedEvent EventState::process(const Event &event,
                                   const QString &streamIdentity)
{
    if (streamIdentity != this->streamIdentity_)
    {
        this->streamIdentity_ = streamIdentity;
        this->users_.clear();
        this->channels_.clear();
        this->badges_.clear();
        if (this->emoteStreamIdentity_ != streamIdentity)
        {
            this->emoteStreamIdentity_.clear();
            this->emotes_.emotes.clear();
        }
        this->userFifo_.clear();
        this->channelFifo_.clear();
    }
    this->deduplicator_.setStreamIdentity(streamIdentity);

    ProcessedEvent output;
    std::visit(
        [this, &output](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, InitEvent> ||
                          std::is_same_v<T, MessagesEvent>)
            {
                if constexpr (std::is_same_v<T, InitEvent>)
                {
                    // Every init is a complete catalog replacement, including
                    // an init that carries no accepted badge definitions.
                    this->badges_.clear();
                    auto badgeIds = typed.badges.keys();
                    std::ranges::sort(badgeIds);
                    const auto keep = std::min<std::size_t>(
                        this->catalogCapacity_,
                        static_cast<std::size_t>(badgeIds.size()));
                    for (std::size_t index = 0; index < keep; ++index)
                    {
                        const auto &id =
                            badgeIds.at(static_cast<qsizetype>(index));
                        this->badges_.insert(id, typed.badges.value(id));
                    }
                }
                this->applyUsers(typed.users);
                this->applyChannels(typed.channels);
                auto messages = typed.messages;
                if constexpr (std::is_same_v<T, InitEvent>)
                {
                    std::stable_sort(
                        messages.begin(), messages.end(),
                        [](const MessageDto &left, const MessageDto &right) {
                            if (left.timestamp != right.timestamp)
                            {
                                return left.timestamp < right.timestamp;
                            }
                            return left.arrivalOrdinal < right.arrivalOrdinal;
                        });
                }

                for (auto &message : messages)
                {
                    this->hydrate(message);
                    if (this->deduplicator_.accept(message.id))
                    {
                        output.messages.push_back(std::move(message));
                    }
                }
            }
            else if constexpr (std::is_same_v<T, PinMessageEvent>)
            {
                auto operation = typed;
                this->hydrate(operation.message);
                output.operations.emplace_back(std::move(operation));
            }
            else
            {
                output.operations.emplace_back(typed);
            }
        },
        event);
    return output;
}

void EventState::applyUsers(const QHash<QString, UserDelta> &deltas)
{
    auto ids = deltas.keys();
    std::ranges::sort(ids);
    for (const auto &id : ids)
    {
        const auto delta = deltas.constFind(id);
        if (!this->users_.contains(id))
        {
            if (this->userFifo_.size() == this->catalogCapacity_)
            {
                this->users_.remove(this->userFifo_.front());
                this->userFifo_.pop_front();
            }
            this->userFifo_.push_back(id);
        }
        auto &stored = this->users_[id];
        stored.id = id;
        mergeOptional(stored.loginName, delta->loginName);
        mergeOptional(stored.displayName, delta->displayName);
        mergeOptional(stored.color, delta->color);
        mergeOptional(stored.badgeIds, delta->badgeIds);
        mergeOptional(stored.roleIds, delta->roleIds);
        mergeOptional(stored.source, delta->source);
    }
}

void EventState::applyChannels(const QHash<QString, ChannelDelta> &deltas)
{
    auto ids = deltas.keys();
    std::ranges::sort(ids);
    for (const auto &id : ids)
    {
        const auto delta = deltas.constFind(id);
        if (!this->channels_.contains(id))
        {
            if (this->channelFifo_.size() == this->catalogCapacity_)
            {
                this->channels_.remove(this->channelFifo_.front());
                this->channelFifo_.pop_front();
            }
            this->channelFifo_.push_back(id);
        }
        auto &stored = this->channels_[id];
        stored.id = id;
        mergeOptional(stored.name, delta->name);
    }
}

void EventState::hydrate(MessageDto &message) const
{
    if (const auto found = this->users_.constFind(message.userId);
        found != this->users_.cend())
    {
        if (!message.loginName)
        {
            message.loginName = found->loginName;
        }
        if (!message.displayName)
        {
            message.displayName = found->displayName;
        }
        if (!message.color)
        {
            message.color = found->color;
        }
        if (!message.badgeIds)
        {
            message.badgeIds = found->badgeIds;
        }
        if (!message.roleIds)
        {
            message.roleIds = found->roleIds;
        }
        if (!message.source)
        {
            message.source = found->source;
        }
    }
    if (const auto found = this->channels_.constFind(message.channelId);
        found != this->channels_.cend() && !message.channelName)
    {
        message.channelName = found->name;
    }

    if (!message.loginName || message.loginName->isEmpty())
    {
        message.loginName = message.userId;
    }
    if (!message.displayName || message.displayName->isEmpty())
    {
        message.displayName = *message.loginName;
    }
    if (!message.color || !message.color->isValid())
    {
        message.color = QColor(153, 153, 153);
    }
    if (!message.channelName || message.channelName->isEmpty())
    {
        // Current Rumble messages commonly omit channel_id. The browser uses
        // the sending user's name in that case.
        if (message.channelId.isEmpty())
        {
            message.channelName = message.loginName;
        }
        else
        {
            message.channelName = message.channelId;
        }
    }
    if (!message.badgeIds)
    {
        message.badgeIds = QStringList{};
    }
    message.resolvedBadges.clear();
    message.resolvedBadges.reserve(
        static_cast<std::size_t>(message.badgeIds->size()));
    for (const auto &id : *message.badgeIds)
    {
        if (id.isEmpty())
        {
            continue;
        }
        const auto definition = this->badges_.constFind(id);
        message.resolvedBadges.push_back({
            .id = id,
            .title = definition == this->badges_.cend() ||
                             definition->title.isEmpty()
                         ? id
                         : definition->title,
            .iconUrl48 = definition == this->badges_.cend()
                             ? QString{}
                             : definition->iconUrl48,
            .iconUrl96 = definition == this->badges_.cend()
                             ? QString{}
                             : definition->iconUrl96,
        });
    }
    message.resolvedEmotes =
        resolveEmoteOccurrences(message.text, this->emotes_);
    if (!message.roleIds)
    {
        message.roleIds = QStringList{};
    }
    if (!message.source)
    {
        message.source = QString{};
    }
}

}  // namespace chatterino::rumble
