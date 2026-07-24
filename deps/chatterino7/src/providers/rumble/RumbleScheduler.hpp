// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class QObject;

namespace chatterino::rumble {

class ScheduledTask
{
public:
    virtual ~ScheduledTask() = default;
    // Lifecycle adapters close their callback gate immediately and dispatch
    // handle destruction/cancellation to the scheduler's owner thread.
    virtual void cancel() noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
};

class RumbleScheduler
{
public:
    using Callback = std::function<void()>;

    virtual ~RumbleScheduler() = default;
    [[nodiscard]] virtual std::int64_t nowMs() const noexcept = 0;
    // The callback must never run inline, including for a zero delay.
    [[nodiscard]] virtual std::unique_ptr<ScheduledTask> scheduleAfter(
        std::int64_t delayMs, Callback callback) = 0;
    // Uniformly selects [0, exclusiveUpperBound). The injectable source makes
    // full-jitter retry schedules deterministic in tests.
    [[nodiscard]] virtual std::uint64_t randomBelow(
        std::uint64_t exclusiveUpperBound) = 0;
};

class QtRumbleScheduler final : public RumbleScheduler
{
public:
    using RandomSource = std::function<std::uint64_t()>;

    explicit QtRumbleScheduler(QObject *owner, RandomSource randomSource = {});
    ~QtRumbleScheduler() override;

    [[nodiscard]] std::int64_t nowMs() const noexcept override;
    [[nodiscard]] std::unique_ptr<ScheduledTask> scheduleAfter(
        std::int64_t delayMs, Callback callback) override;
    [[nodiscard]] std::uint64_t randomBelow(
        std::uint64_t exclusiveUpperBound) override;

private:
    struct State;
    std::shared_ptr<State> state_;
    RandomSource randomSource_;
};

}  // namespace chatterino::rumble
