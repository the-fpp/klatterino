// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#include "util/Expected.hpp"

#include <QString>

#include <cstddef>
#include <cstdint>

namespace chatterino {
enum class RumbleChannelKeyKind : std::uint8_t {
    ChannelSlug,
    EmbedId,
    StreamId,
};
enum class RumbleChannelKeyError : std::uint8_t {
    Empty,
    InvalidSlug,
    InvalidEmbedId,
    InvalidStreamId,
};
class RumbleChannelKey
{
public:
    static Expected<RumbleChannelKey, RumbleChannelKeyError> normalize(
        RumbleChannelKeyKind kind, QString value);
    RumbleChannelKeyKind kind() const noexcept;
    const QString &value() const noexcept;
    friend bool operator==(const RumbleChannelKey &,
                           const RumbleChannelKey &) = default;

private:
    RumbleChannelKey(RumbleChannelKeyKind kind, QString value);
    RumbleChannelKeyKind kind_;
    QString value_;
};
struct RumbleChannelKeyHash {
    std::size_t operator()(const RumbleChannelKey &key) const noexcept;
};
}  // namespace chatterino
