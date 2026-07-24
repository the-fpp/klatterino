// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT
#include "providers/rumble/RumbleChannelKey.hpp"

#include <QChar>
#include <QHashFunctions>

#include <utility>

namespace chatterino {
namespace {
void lowercaseAscii(QString &value)
{
    for (qsizetype index = 0; index < value.size(); ++index)
    {
        const auto code = value.at(index).unicode();
        if (code >= u'A' && code <= u'Z')
        {
            value[index] = QChar(code + static_cast<char16_t>(u'a' - u'A'));
        }
    }
}
bool validSlug(QStringView value)
{
    for (const auto character : value)
    {
        const auto code = character.unicode();
        if (code < 0x20 || character.isSpace() || character == u'/' ||
            character == u'\\' || character == u'?' || character == u'#')
        {
            return false;
        }
    }
    return !value.isEmpty();
}
bool validEmbed(QStringView value)
{
    if (value.size() < 2 || value.front() != u'v')
    {
        return false;
    }
    for (const auto character : value.sliced(1))
    {
        if (!((character >= u'0' && character <= u'9') ||
              (character >= u'a' && character <= u'z')))
        {
            return false;
        }
    }
    return true;
}
}  // namespace
RumbleChannelKey::RumbleChannelKey(RumbleChannelKeyKind kind, QString value)
    : kind_(kind)
    , value_(std::move(value))
{
}
Expected<RumbleChannelKey, RumbleChannelKeyError> RumbleChannelKey::normalize(
    RumbleChannelKeyKind kind, QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return makeUnexpected(RumbleChannelKeyError::Empty);
    }
    switch (kind)
    {
        case RumbleChannelKeyKind::ChannelSlug:
            lowercaseAscii(value);
            if (!validSlug(value))
            {
                return makeUnexpected(RumbleChannelKeyError::InvalidSlug);
            }
            break;
        case RumbleChannelKeyKind::EmbedId:
            lowercaseAscii(value);
            if (!validEmbed(value))
            {
                return makeUnexpected(RumbleChannelKeyError::InvalidEmbedId);
            }
            break;
        case RumbleChannelKeyKind::StreamId: {
            for (const auto character : value)
            {
                if (character < u'0' || character > u'9')
                {
                    return makeUnexpected(
                        RumbleChannelKeyError::InvalidStreamId);
                }
            }
            qsizetype firstNonZero = 0;
            while (firstNonZero < value.size() && value[firstNonZero] == u'0')
            {
                ++firstNonZero;
            }
            if (firstNonZero == value.size())
            {
                return makeUnexpected(RumbleChannelKeyError::InvalidStreamId);
            }
            value = value.sliced(firstNonZero);
            break;
        }
    }
    return RumbleChannelKey(kind, std::move(value));
}
RumbleChannelKeyKind RumbleChannelKey::kind() const noexcept
{
    return this->kind_;
}
const QString &RumbleChannelKey::value() const noexcept
{
    return this->value_;
}
std::size_t RumbleChannelKeyHash::operator()(
    const RumbleChannelKey &key) const noexcept
{
    const auto valueHash = static_cast<std::size_t>(qHash(key.value()));
    const auto kindHash = static_cast<std::size_t>(key.kind());
    return valueHash ^
           (kindHash + 0x9e3779b9U + (valueHash << 6U) + (valueHash >> 2U));
}
}  // namespace chatterino
