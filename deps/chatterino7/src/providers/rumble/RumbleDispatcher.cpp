// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT
#include "providers/rumble/RumbleDispatcher.hpp"

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <cassert>
#include <mutex>
#include <utility>
#include <vector>

namespace chatterino {
namespace {
class OwnerCleanupState
{
public:
    explicit OwnerCleanupState(RumbleDispatcher::Task cleanup)
        : cleanup_(std::move(cleanup))
    {
    }
    ~OwnerCleanupState()
    {
        run();
    }
    void run() noexcept
    {
        if (!cleanup_)
            return;
        auto cleanup = std::move(cleanup_);
        cleanup_ = {};
        cleanup();
    }
    RumbleDispatcher::Task take() noexcept
    {
        return std::exchange(cleanup_, {});
    }

private:
    RumbleDispatcher::Task cleanup_;
};

class QtRumbleDispatcher final : public RumbleDispatcher
{
public:
    explicit QtRumbleDispatcher(QObject *owner)
        : receiver_(new QObject)
    {
        assert(owner != nullptr);
        if (this->receiver_->thread() != owner->thread())
        {
            this->receiver_->moveToThread(owner->thread());
        }
    }
    ~QtRumbleDispatcher() override
    {
        std::vector<Task> emergency;
        {
            std::lock_guard lock(this->mutex_);
            emergency = std::move(this->emergencyCleanup_);
        }
        auto *receiver = std::exchange(this->receiver_, nullptr);
        if (receiver == nullptr)
            return;
        if (!emergency.empty())
        {
            auto state = std::make_shared<OwnerCleanupState>(
                [emergency = std::move(emergency)]() mutable {
                    for (auto &task : emergency)
                        task();
                    emergency.clear();
                });
            const bool accepted = QMetaObject::invokeMethod(
                receiver,
                [state] {
                    state->run();
                },
                Qt::QueuedConnection);
            if (!accepted)
            {
                QObject::connect(receiver, &QObject::destroyed, [state] {
                    state->run();
                });
            }
        }
        // deleteLater is thread-safe and keeps QObject destruction on the
        // receiver's affinity thread even when the final dispatcher reference
        // is released by a producer.
        receiver->deleteLater();
    }
    bool isOwnerThread() const noexcept override
    {
        return this->receiver_ != nullptr &&
               this->receiver_->thread() == QThread::currentThread();
    }
    bool dispatch(Task task) override
    {
        if (this->receiver_ == nullptr)
            return false;
        auto keepAlive = this->shared_from_this();
        return QMetaObject::invokeMethod(
            this->receiver_,
            [keepAlive = std::move(keepAlive),
             task = std::move(task)]() mutable {
                task();
            },
            Qt::QueuedConnection);
    }
    void dispose(Task cleanup) noexcept override
    {
        if (this->isOwnerThread())
        {
            cleanup();
            return;
        }
        auto state = std::make_shared<OwnerCleanupState>(std::move(cleanup));
        auto keepAlive = this->shared_from_this();
        const bool accepted = this->receiver_ != nullptr &&
                              QMetaObject::invokeMethod(
                                  this->receiver_,
                                  [keepAlive = std::move(keepAlive), state] {
                                      state->run();
                                  },
                                  Qt::QueuedConnection);
        if (!accepted)
        {
            std::lock_guard lock(this->mutex_);
            this->emergencyCleanup_.emplace_back(state->take());
        }
    }

private:
    QObject *receiver_;
    std::mutex mutex_;
    std::vector<Task> emergencyCleanup_;
};
}  // namespace
std::shared_ptr<RumbleDispatcher> makeQtRumbleDispatcher(QObject *owner)
{
    return std::make_shared<QtRumbleDispatcher>(owner);
}
}  // namespace chatterino
