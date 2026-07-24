// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleChannelKey.hpp"
#include "util/Expected.hpp"

#include <QString>

#include <cstdint>

namespace chatterino {

enum class RumbleLayoutLocatorError : std::uint8_t {
    InvalidOrUnsupported,
    DirectStreamNotPersistable,
};

/// Canonical, public, persistence-safe Rumble locator.  Construction rejects
/// stream IDs because they are transient transport identities and cannot be
/// restored reliably.
class RumbleLayoutLocator
{
public:
    /// Permissive interactive parser. Picker input may be a supported bare
    /// channel slug; successful values are always returned as canonical HTTPS
    /// URLs before they enter layout state.
    static Expected<RumbleLayoutLocator, RumbleLayoutLocatorError>
        fromUserInput(const QString &input);

    /// Trust-boundary parser for layout, stream, and retry state. Persisted
    /// locators must already be explicit HTTPS Rumble URLs; query and fragment
    /// components are removed while canonicalizing.
    static Expected<RumbleLayoutLocator, RumbleLayoutLocatorError>
        fromPersisted(const QString &input);

    const QString &canonicalUrl() const noexcept;
    const RumbleChannelKey &channelKey() const noexcept;
    rumble::LocatorKind kind() const noexcept;

    friend bool operator==(const RumbleLayoutLocator &,
                           const RumbleLayoutLocator &) = default;

private:
    RumbleLayoutLocator(QString canonicalUrl, RumbleChannelKey channelKey,
                        rumble::LocatorKind kind);

    QString canonicalUrl_;
    RumbleChannelKey channelKey_;
    rumble::LocatorKind kind_;
};

QString rumbleLayoutErrorText(RumbleLayoutLocatorError error);

}  // namespace chatterino
