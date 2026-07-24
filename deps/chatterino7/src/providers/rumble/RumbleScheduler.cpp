// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleScheduler.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chatterino::rumble {

struct QtRumbleScheduler::State {
    QPointer<QObject> owner;
    QElapsedTimer clock;
    std::atomic_bool alive{true};
};

namespace {

struct QtTaskState {
    QPointer<QTimer> timer;
    std::atomic_bool active{true};
    std::int64_t remainingMs = 0;
    int currentChunkMs = 0;
};

class QtScheduledTask final : public ScheduledTask
{
public:
    explicit QtScheduledTask(std::shared_ptr<QtTaskState> state)
        : state_(std::move(state))
    {
    }

    ~QtScheduledTask() override
    {
        this->cancel();
    }

    void cancel() noexcept override
    {
        auto state = std::exchange(this->state_, {});
        if (!state || !state->active.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }
        const QPointer<QTimer> timer = state->timer;
        if (!timer)
        {
            return;
        }
        if (timer->thread() == QThread::currentThread())
        {
            timer->stop();
            timer->deleteLater();
            return;
        }
        QMetaObject::invokeMethod(
            timer,
            [timer] {
                if (timer)
                {
                    timer->stop();
                    timer->deleteLater();
                }
            },
            Qt::QueuedConnection);
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return this->state_ &&
               this->state_->active.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<QtTaskState> state_;
};

}  // namespace

QtRumbleScheduler::QtRumbleScheduler(QObject *owner, RandomSource randomSource)
    : state_(std::make_shared<State>())
    , randomSource_(std::move(randomSource))
{
    if (owner == nullptr)
    {
        throw std::invalid_argument("QtRumbleScheduler requires an owner");
    }
    this->state_->owner = owner;
    this->state_->clock.start();
    if (!this->randomSource_)
    {
        this->randomSource_ = [] {
            return QRandomGenerator::global()->generate64();
        };
    }
}

QtRumbleScheduler::~QtRumbleScheduler()
{
    this->state_->alive.store(false, std::memory_order_release);
    this->state_->owner = nullptr;
}

std::int64_t QtRumbleScheduler::nowMs() const noexcept
{
    return this->state_->clock.elapsed();
}

std::unique_ptr<ScheduledTask> QtRumbleScheduler::scheduleAfter(
    std::int64_t delayMs, Callback callback)
{
    if (delayMs < 0 || !callback || !this->state_->owner ||
        this->state_->owner->thread() != QThread::currentThread())
    {
        return nullptr;
    }

    auto task = std::make_shared<QtTaskState>();
    task->remainingMs = delayMs;
    task->currentChunkMs = static_cast<int>(
        std::min<std::int64_t>(delayMs, std::numeric_limits<int>::max()));
    auto *timer = new QTimer(this->state_->owner);
    task->timer = timer;
    timer->setSingleShot(true);
    const std::weak_ptr weakScheduler = this->state_;
    const std::weak_ptr weakTask = task;
    QObject::connect(
        timer, &QTimer::timeout, this->state_->owner,
        [weakScheduler, weakTask, callback = std::move(callback)]() mutable {
            const auto scheduler = weakScheduler.lock();
            const auto lockedTask = weakTask.lock();
            if (!lockedTask)
                return;
            if (!scheduler || !scheduler->alive.load(std::memory_order_acquire))
            {
                lockedTask->active.store(false, std::memory_order_release);
                if (lockedTask->timer)
                {
                    lockedTask->timer->deleteLater();
                    lockedTask->timer = nullptr;
                }
                return;
            }
            if (!lockedTask->active.load(std::memory_order_acquire))
                return;
            lockedTask->remainingMs -= lockedTask->currentChunkMs;
            if (lockedTask->remainingMs > 0)
            {
                lockedTask->currentChunkMs = static_cast<int>(
                    std::min<std::int64_t>(lockedTask->remainingMs,
                                           std::numeric_limits<int>::max()));
                if (lockedTask->timer)
                    lockedTask->timer->start(lockedTask->currentChunkMs);
                return;
            }
            if (!lockedTask->active.exchange(false, std::memory_order_acq_rel))
                return;
            if (lockedTask->timer)
            {
                lockedTask->timer->deleteLater();
                lockedTask->timer = nullptr;
            }
            callback();
        });
    const std::weak_ptr destroyedTask = task;
    QObject::connect(timer, &QObject::destroyed, [destroyedTask] {
        if (const auto locked = destroyedTask.lock())
        {
            locked->timer = nullptr;
            locked->active.store(false, std::memory_order_release);
        }
    });
    timer->start(task->currentChunkMs);
    return std::make_unique<QtScheduledTask>(std::move(task));
}

std::uint64_t QtRumbleScheduler::randomBelow(std::uint64_t exclusiveUpperBound)
{
    if (exclusiveUpperBound == 0)
    {
        return 0;
    }
    // Rejection sampling avoids modulo bias while retaining an injectable,
    // deterministic 64-bit source.
    const auto limit =
        std::numeric_limits<std::uint64_t>::max() -
        (std::numeric_limits<std::uint64_t>::max() % exclusiveUpperBound);
    std::uint64_t value = 0;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        value = this->randomSource_();
        if (value < limit)
            return value % exclusiveUpperBound;
    }
    // A pathological injected source cannot spin the owner thread forever.
    // The bounded fallback remains within the required jitter interval.
    return value % exclusiveUpperBound;
}

}  // namespace chatterino::rumble
