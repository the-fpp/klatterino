// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace chatterino {

class RumbleChannel;
class RumbleDispatcher;

namespace rumble {

class RumbleApi;
class RumbleScheduler;

// Owns public resolution and one anonymous SSE reader for one channel. API,
// scheduler, and dispatcher must outlive the controller. All provider work is
// owner-thread-affine; the channel-owned cancellation adapter closes its
// callback gate immediately when cancellation originates on another thread.
class RumbleConnection final
{
public:
    struct Options {
        std::size_t deduplicationCapacity = 50000;
        std::int64_t offlineRecheckMs = 30 * 1000;
        std::uint32_t maximumConsecutiveFailures = 3;
        std::int64_t maximumBackoffMs = 30 * 1000;
    };

    RumbleConnection(std::shared_ptr<RumbleChannel> channel, RumbleApi &api,
                     RumbleScheduler &scheduler,
                     std::shared_ptr<RumbleDispatcher> dispatcher,
                     QString locator);
    RumbleConnection(std::shared_ptr<RumbleChannel> channel, RumbleApi &api,
                     RumbleScheduler &scheduler,
                     std::shared_ptr<RumbleDispatcher> dispatcher,
                     QString locator, Options options);
    ~RumbleConnection();

    RumbleConnection(const RumbleConnection &) = delete;
    RumbleConnection &operator=(const RumbleConnection &) = delete;
    RumbleConnection(RumbleConnection &&) = delete;
    RumbleConnection &operator=(RumbleConnection &&) = delete;

    // Starts once. Returns false for a wrong-thread/closed/invalid owner seam.
    bool start();
    // Explicit retry keeps the locator and begins a new lifecycle generation.
    void retry();
    // Account/context replacement cancels first, then resolves the new locator.
    void restart(QString locator);
    // Cancels transport/timers and removes reconnect ownership without closing
    // the channel. Application shutdown uses this before serializing layout.
    void retire();
    void stop();

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace rumble
}  // namespace chatterino
