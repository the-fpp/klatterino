// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#include <functional>
#include <memory>

class QObject;

namespace chatterino {
class RumbleDispatcher : public std::enable_shared_from_this<RumbleDispatcher>
{
public:
    using Task = std::function<void()>;
    virtual ~RumbleDispatcher() = default;
    virtual bool isOwnerThread() const noexcept = 0;
    // Enqueues the task for a later turn, even when called on the owner thread,
    // and returns whether ownership was accepted. Implementations must never
    // invoke the task inline. A false result means the owner is no longer
    // available and the task will never run.
    virtual bool dispatch(Task task) = 0;
    // Transfers an owner-affine cleanup to the dispatcher. The cleanup object
    // itself is released only on the owner thread, whether its queued call runs
    // or is discarded while the private receiver is destroyed.
    virtual void dispose(Task cleanup) noexcept = 0;
};
std::shared_ptr<RumbleDispatcher> makeQtRumbleDispatcher(QObject *owner);
}  // namespace chatterino
