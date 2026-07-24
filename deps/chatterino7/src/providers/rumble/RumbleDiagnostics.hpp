// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleSession.hpp"

#include <QDateTime>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace chatterino::rumble {

enum class StatusState : std::uint8_t {
    Offline,
    Connecting,
    Connected,
    Backoff,
    RateLimited,
    Error,
    Stopped,
};

enum class StatusLocator : std::uint8_t { Channel, Embed, Stream };

// Rumble accounts support validated sessions and sending. There is no separate
// "unsupported" state in that merged contract.
enum class StatusAccount : std::uint8_t {
    LoggedOut,
    NeedsValidation,
    Validating,
    Authenticated,
};

enum class StatusWrite : std::uint8_t {
    Unavailable,
    Writable,
    Busy,
    RateLimited,
    DestinationDenied,
};

struct StatusSnapshot {
    StatusState state = StatusState::Stopped;
    StatusLocator locator = StatusLocator::Channel;
    StatusAccount account = StatusAccount::LoggedOut;
    StatusWrite write = StatusWrite::Unavailable;
    std::uint32_t consecutiveFailures = 0;
    std::optional<std::int64_t> retryWaitMs;
    std::optional<RumbleRetryCause> retryCause;
    std::optional<RumbleFailureCategory> lastError;
    std::optional<QDateTime> lastErrorAtUtc;

    friend bool operator==(const StatusSnapshot &,
                           const StatusSnapshot &) = default;
};

/// Copies only closed-vocabulary enums, bounded counters, and UTC times. No
/// raw locator, account, user, message, request, URL, payload, or error text
/// can enter the public diagnostic boundary.
std::optional<StatusSnapshot> captureStatus(const RumbleChannel &channel,
                                            QDateTime nowUtc);

/// Returns one concise, public-issue-safe status block. This is a pure
/// formatter and performs no network, timer, account, or channel operation.
QString formatStatus(const StatusSnapshot &snapshot);

/// Fail-closed vocabulary seam used by tests and any future diagnostic fields.
/// Unknown or hostile input can never be reflected into output.
QString sanitizeStatusToken(QString value);

struct StatusLogKey {
    StatusState state = StatusState::Stopped;
    StatusAccount account = StatusAccount::LoggedOut;
    StatusWrite write = StatusWrite::Unavailable;
    StatusLocator locator = StatusLocator::Channel;
    std::optional<RumbleFailureCategory> error;
    std::optional<RumbleRetryCause> retryCause;

    friend bool operator==(const StatusLogKey &,
                           const StatusLogKey &) = default;
};

struct StatusLogDecision {
    bool emit = false;
    std::uint32_t suppressed = 0;

    friend bool operator==(const StatusLogDecision &,
                           const StatusLogDecision &) = default;
};

/// Bounded, scalar-only coalescing state. The instance owns no channel,
/// account, request, widget, credential buffer, or free-form string.
class StatusLogCoalescer
{
public:
    explicit StatusLogCoalescer(std::int64_t intervalMs = 60 * 1000,
                                std::size_t maximumKeys = 64);
    StatusLogDecision observe(StatusLogKey key, std::int64_t nowMs);
    std::size_t size() const noexcept;

private:
    struct Entry {
        StatusLogKey key;
        std::int64_t lastEmissionMs = 0;
        std::int64_t lastObservationMs = 0;
        std::uint32_t suppressed = 0;
    };
    std::int64_t intervalMs_;
    std::size_t maximumKeys_;
    std::vector<Entry> entries_;
    std::int64_t windowStartMs_ = 0;
    std::size_t emissionsInWindow_ = 0;
    std::uint32_t carriedSuppressed_ = 0;
    std::optional<std::int64_t> lastNowMs_;
};

/// Emits one structured record through the dedicated chatterino.rumble
/// category after global bounded coalescing. The record is built exclusively
/// from StatusSnapshot's sanitized scalar vocabulary.
void logStatus(const StatusSnapshot &snapshot, std::int64_t nowMs);

}  // namespace chatterino::rumble
