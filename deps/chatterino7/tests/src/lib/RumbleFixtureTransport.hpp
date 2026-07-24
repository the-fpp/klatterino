// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chatterino::test {

class ManualScheduler
{
public:
    using TaskId = std::uint64_t;
    using Callback = std::function<void()>;

    [[nodiscard]] std::int64_t nowMs() const noexcept;
    [[nodiscard]] std::size_t pendingTaskCount() const noexcept;

    TaskId scheduleAfter(std::int64_t delayMs, Callback callback);
    void cancel(TaskId id) noexcept;

    std::size_t runReady();
    std::size_t advanceBy(std::int64_t deltaMs);
    std::size_t runUntilIdle();

private:
    struct Task {
        TaskId id;
        std::int64_t dueMs;
        std::uint64_t order;
        Callback callback;
    };

    std::int64_t nowMs_ = 0;
    TaskId nextTaskId_ = 1;
    std::uint64_t nextOrder_ = 0;
    std::vector<Task> tasks_;
};

struct RumbleFixtureHeader {
    std::string name;
    std::string value;
};

[[nodiscard]] const std::string *findHeader(
    const std::vector<RumbleFixtureHeader> &headers,
    const std::string &name) noexcept;

struct RumbleFixtureRequest {
    std::string method;
    std::string target;
    std::vector<RumbleFixtureHeader> headers;
};

struct RumbleFixtureResponseHead {
    int status = 0;
    std::vector<RumbleFixtureHeader> headers;
};

struct RumbleFixtureChunk {
    // Delay relative to the response head or preceding chunk.
    std::int64_t afterMs = 0;
    std::string bytes;
};

enum class RumbleFixtureTerminal {
    Complete,
    Disconnect,
};

struct RumbleFixtureExchange {
    std::string label;
    RumbleFixtureRequest expectedRequest;
    RumbleFixtureResponseHead response;
    std::int64_t headAfterMs = 0;
    std::vector<RumbleFixtureChunk> chunks;
    RumbleFixtureTerminal terminal = RumbleFixtureTerminal::Complete;
    std::int64_t terminalAfterMs = 0;
    std::string terminalReason;
};

struct RumbleFixtureScript {
    std::string name;
    std::vector<RumbleFixtureExchange> exchanges;
};

struct RumbleFixtureCallbacks {
    std::function<void(const RumbleFixtureResponseHead &)> onHead;
    std::function<void(const std::string &)> onBodyChunk;
    std::function<void()> onComplete;
    std::function<void(const std::string &)> onDisconnect;
};

namespace detail {
struct RumbleFixtureOperation;
struct RumbleFixtureTransportState;
}  // namespace detail

class RumbleFixtureRequestHandle
{
public:
    RumbleFixtureRequestHandle() = default;
    RumbleFixtureRequestHandle(const RumbleFixtureRequestHandle &) = delete;
    RumbleFixtureRequestHandle &operator=(
        const RumbleFixtureRequestHandle &) = delete;
    RumbleFixtureRequestHandle(RumbleFixtureRequestHandle &&other) noexcept;
    RumbleFixtureRequestHandle &operator=(
        RumbleFixtureRequestHandle &&other) noexcept;
    ~RumbleFixtureRequestHandle();

    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    RumbleFixtureRequestHandle(
        std::weak_ptr<detail::RumbleFixtureTransportState> transport,
        std::weak_ptr<detail::RumbleFixtureOperation> operation);

    std::weak_ptr<detail::RumbleFixtureTransportState> transport_;
    std::weak_ptr<detail::RumbleFixtureOperation> operation_;

    friend class RumbleFixtureTransport;
};

class RumbleFixtureTransport
{
public:
    RumbleFixtureTransport(ManualScheduler &scheduler,
                           RumbleFixtureScript script);
    RumbleFixtureTransport(const RumbleFixtureTransport &) = delete;
    RumbleFixtureTransport &operator=(const RumbleFixtureTransport &) = delete;
    RumbleFixtureTransport(RumbleFixtureTransport &&) = delete;
    RumbleFixtureTransport &operator=(RumbleFixtureTransport &&) = delete;
    ~RumbleFixtureTransport();

    // Throws std::logic_error without consuming the exchange when the method,
    // target, or required request headers do not match the next script entry.
    RumbleFixtureRequestHandle start(RumbleFixtureRequest request,
                                     RumbleFixtureCallbacks callbacks);

    void cancelAll() noexcept;
    [[nodiscard]] std::size_t activeRequestCount() const noexcept;
    [[nodiscard]] std::size_t remainingExchangeCount() const noexcept;

private:
    std::shared_ptr<detail::RumbleFixtureTransportState> state_;
};

}  // namespace chatterino::test
