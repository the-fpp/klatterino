// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#include "providers/rumble/RumbleChannelKey.hpp"
#include "util/Expected.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace chatterino {
class RumbleChannel;
class RumbleDispatcher;
enum class RumbleProviderErrorCode : std::uint8_t {
    InvalidKey,
    WrongThread,
    Shutdown,
    ForeignChannel,
    ClosedChannel,
    AliasConflict,
};
struct RumbleProviderError {
    RumbleProviderErrorCode code;
    std::optional<RumbleChannelKeyError> keyError;
    friend bool operator==(const RumbleProviderError &,
                           const RumbleProviderError &) = default;
};
class RumbleChannelProvider
{
public:
    explicit RumbleChannelProvider(
        std::shared_ptr<RumbleDispatcher> dispatcher);
    ~RumbleChannelProvider();
    RumbleChannelProvider(const RumbleChannelProvider &) = delete;
    RumbleChannelProvider &operator=(const RumbleChannelProvider &) = delete;
    Expected<std::shared_ptr<RumbleChannel>, RumbleProviderError> getOrCreate(
        RumbleChannelKeyKind kind, QString value);
    Expected<std::shared_ptr<RumbleChannel>, RumbleProviderError> getOrCreate(
        const RumbleChannelKey &key);
    Expected<void, RumbleProviderError> associateAlias(
        const std::shared_ptr<RumbleChannel> &channel,
        RumbleChannelKeyKind kind, QString value);
    Expected<void, RumbleProviderError> associateAlias(
        const std::shared_ptr<RumbleChannel> &channel,
        const RumbleChannelKey &alias);
    void shutdown();
    bool isShutdown() const;

private:
    using Cache =
        std::unordered_map<RumbleChannelKey, std::weak_ptr<RumbleChannel>,
                           RumbleChannelKeyHash>;
    std::shared_ptr<RumbleDispatcher> dispatcher_;
    std::shared_ptr<const void> identity_;
    mutable std::mutex mutex_;
    Cache cache_;
    bool shutdown_ = false;
};
}  // namespace chatterino
