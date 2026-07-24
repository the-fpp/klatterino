// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleLayoutLocator.hpp"

#include <QUrl>

#include <utility>

namespace chatterino {

namespace {

QString encodedPathSegment(const QString &value)
{
    return QString::fromLatin1(
        QUrl::toPercentEncoding(value, QByteArrayLiteral("-._~")));
}

}  // namespace

RumbleLayoutLocator::RumbleLayoutLocator(QString canonicalUrl,
                                         RumbleChannelKey channelKey,
                                         rumble::LocatorKind kind)
    : canonicalUrl_(std::move(canonicalUrl))
    , channelKey_(std::move(channelKey))
    , kind_(kind)
{
}

Expected<RumbleLayoutLocator, RumbleLayoutLocatorError>
    RumbleLayoutLocator::fromUserInput(const QString &input)
{
    auto locator = rumble::RumbleApi::normalizeLocator(input);
    if (!locator)
    {
        return makeUnexpected(RumbleLayoutLocatorError::InvalidOrUnsupported);
    }
    if (locator->kind == rumble::LocatorKind::Stream)
    {
        return makeUnexpected(
            RumbleLayoutLocatorError::DirectStreamNotPersistable);
    }

    auto key = RumbleChannelKey::normalize(
        locator->kind == rumble::LocatorKind::Channel
            ? RumbleChannelKeyKind::ChannelSlug
            : RumbleChannelKeyKind::EmbedId,
        locator->value);
    if (!key)
    {
        return makeUnexpected(RumbleLayoutLocatorError::InvalidOrUnsupported);
    }

    QString canonical;
    switch (locator->kind)
    {
        case rumble::LocatorKind::Channel:
            canonical = QStringLiteral("https://rumble.com/c/%1")
                            .arg(encodedPathSegment(key->value()));
            break;
        case rumble::LocatorKind::Video:
            canonical = QStringLiteral("https://rumble.com/embed/%1")
                            .arg(encodedPathSegment(key->value()));
            break;
        case rumble::LocatorKind::VideoPage:
            canonical =
                QStringLiteral("https://rumble.com%1").arg(locator->pagePath);
            break;
        case rumble::LocatorKind::Stream:
            return makeUnexpected(
                RumbleLayoutLocatorError::DirectStreamNotPersistable);
    }

    return RumbleLayoutLocator(std::move(canonical), std::move(*key),
                               locator->kind);
}

Expected<RumbleLayoutLocator, RumbleLayoutLocatorError>
    RumbleLayoutLocator::fromPersisted(const QString &input)
{
    const auto trimmed = input.trimmed();
    const QUrl url(trimmed, QUrl::StrictMode);
    const auto host = url.host().toLower();
    if (!url.isValid() ||
        url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) !=
            0 ||
        (host != QStringLiteral("rumble.com") &&
         host != QStringLiteral("www.rumble.com")) ||
        !url.userInfo().isEmpty() || url.port(-1) != -1)
    {
        return makeUnexpected(RumbleLayoutLocatorError::InvalidOrUnsupported);
    }
    return fromUserInput(trimmed);
}

const QString &RumbleLayoutLocator::canonicalUrl() const noexcept
{
    return this->canonicalUrl_;
}

const RumbleChannelKey &RumbleLayoutLocator::channelKey() const noexcept
{
    return this->channelKey_;
}

rumble::LocatorKind RumbleLayoutLocator::kind() const noexcept
{
    return this->kind_;
}

QString rumbleLayoutErrorText(RumbleLayoutLocatorError error)
{
    switch (error)
    {
        case RumbleLayoutLocatorError::InvalidOrUnsupported:
            return QStringLiteral(
                "Enter a public Rumble channel, video, or video-page URL.");
        case RumbleLayoutLocatorError::DirectStreamNotPersistable:
            return QStringLiteral(
                "This Rumble link can't be saved. Use a public channel or "
                "video URL instead.");
    }
    return QStringLiteral("Enter a public Rumble channel or video URL.");
}

}  // namespace chatterino
