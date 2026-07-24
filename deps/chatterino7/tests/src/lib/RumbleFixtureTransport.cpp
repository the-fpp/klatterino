// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureTransport.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chatterino::test {
namespace {

bool equalHeaderName(const std::string &lhs, const std::string &rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    return std::ranges::equal(lhs, rhs, [](unsigned char left,
                                           unsigned char right) {
        return std::tolower(left) == std::tolower(right);
    });
}

}  // namespace

std::int64_t ManualScheduler::nowMs() const noexcept
{
    return this->nowMs_;
}

std::size_t ManualScheduler::pendingTaskCount() const noexcept
{
    return this->tasks_.size();
}

ManualScheduler::TaskId ManualScheduler::scheduleAfter(
    std::int64_t delayMs, Callback callback)
{
    if (delayMs < 0)
    {
        throw std::invalid_argument(
            "manual scheduler delay must be non-negative");
    }
    if (!callback)
    {
        throw std::invalid_argument("manual scheduler callback must be set");
    }
    if (delayMs > std::numeric_limits<std::int64_t>::max() - this->nowMs_)
    {
        throw std::overflow_error("manual scheduler deadline overflow");
    }

    const auto id = this->nextTaskId_++;
    this->tasks_.push_back({
        .id = id,
        .dueMs = this->nowMs_ + delayMs,
        .order = this->nextOrder_++,
        .callback = std::move(callback),
    });
    return id;
}

void ManualScheduler::cancel(TaskId id) noexcept
{
    std::erase_if(this->tasks_, [id](const Task &task) {
        return task.id == id;
    });
}

std::size_t ManualScheduler::runReady()
{
    std::size_t executed = 0;
    while (true)
    {
        auto next = this->tasks_.end();
        for (auto it = this->tasks_.begin(); it != this->tasks_.end(); ++it)
        {
            if (it->dueMs > this->nowMs_)
            {
                continue;
            }
            if (next == this->tasks_.end() || it->dueMs < next->dueMs ||
                (it->dueMs == next->dueMs && it->order < next->order))
            {
                next = it;
            }
        }

        if (next == this->tasks_.end())
        {
            break;
        }

        auto callback = std::move(next->callback);
        this->tasks_.erase(next);
        callback();
        ++executed;
    }
    return executed;
}

std::size_t ManualScheduler::advanceBy(std::int64_t deltaMs)
{
    if (deltaMs < 0)
    {
        throw std::invalid_argument(
            "manual scheduler cannot advance by a negative duration");
    }
    if (deltaMs > std::numeric_limits<std::int64_t>::max() - this->nowMs_)
    {
        throw std::overflow_error("manual scheduler time overflow");
    }

    const auto targetMs = this->nowMs_ + deltaMs;
    std::size_t executed = 0;
    while (true)
    {
        const auto next = std::ranges::min_element(
            this->tasks_, {}, &Task::dueMs);
        if (next == this->tasks_.end() || next->dueMs > targetMs)
        {
            break;
        }

        // Execute at each task's actual deadline so callbacks which schedule
        // relative work observe the same clock regardless of advance size.
        this->nowMs_ = std::max(this->nowMs_, next->dueMs);
        executed += this->runReady();
    }

    this->nowMs_ = targetMs;
    return executed;
}

std::size_t ManualScheduler::runUntilIdle()
{
    std::size_t executed = 0;
    while (!this->tasks_.empty())
    {
        const auto next = std::ranges::min_element(
            this->tasks_, {}, &Task::dueMs);
        this->nowMs_ = std::max(this->nowMs_, next->dueMs);
        executed += this->runReady();
    }
    return executed;
}

const std::string *findHeader(
    const std::vector<RumbleFixtureHeader> &headers,
    const std::string &name) noexcept
{
    const auto found = std::ranges::find_if(headers, [&](const auto &header) {
        return equalHeaderName(header.name, name);
    });
    return found == headers.end() ? nullptr : &found->value;
}

namespace detail {

struct RumbleFixtureOperation {
    bool active = true;
    RumbleFixtureCallbacks callbacks;
    std::vector<ManualScheduler::TaskId> taskIds;
};

struct RumbleFixtureTransportState
    : std::enable_shared_from_this<RumbleFixtureTransportState> {
    RumbleFixtureTransportState(ManualScheduler &scheduler,
                                RumbleFixtureScript script)
        : scheduler(scheduler)
        , script(std::move(script))
    {
    }

    void retire(
        const std::shared_ptr<RumbleFixtureOperation> &operation) noexcept
    {
        std::erase(this->operations, operation);
    }

    void cancel(
        const std::shared_ptr<RumbleFixtureOperation> &operation) noexcept
    {
        if (!operation || !operation->active)
        {
            return;
        }

        operation->active = false;
        for (const auto id : operation->taskIds)
        {
            this->scheduler.cancel(id);
        }
        operation->taskIds.clear();
        operation->callbacks = {};
        this->retire(operation);
    }

    void cancelAll() noexcept
    {
        const auto operations = this->operations;
        for (const auto &operation : operations)
        {
            this->cancel(operation);
        }
    }

    ManualScheduler &scheduler;
    RumbleFixtureScript script;
    std::size_t nextExchange = 0;
    std::vector<std::shared_ptr<RumbleFixtureOperation>> operations;
};

}  // namespace detail

namespace {

std::string describeRequest(const RumbleFixtureRequest &request)
{
    return request.method + " " + request.target;
}

void validateRequest(const RumbleFixtureRequest &actual,
                     const RumbleFixtureRequest &expected,
                     const std::string &label)
{
    if (actual.method != expected.method || actual.target != expected.target)
    {
        throw std::logic_error(
            "fixture exchange '" + label + "' expected " +
            describeRequest(expected) + ", received " +
            describeRequest(actual));
    }

    for (const auto &required : expected.headers)
    {
        const auto *actualValue = findHeader(actual.headers, required.name);
        if (actualValue == nullptr || *actualValue != required.value)
        {
            throw std::logic_error("fixture exchange '" + label +
                                   "' request header mismatch: " +
                                   required.name);
        }
    }
}

template <typename Callback>
void scheduleOperationTask(
    const std::shared_ptr<detail::RumbleFixtureTransportState> &state,
    const std::shared_ptr<detail::RumbleFixtureOperation> &operation,
    std::int64_t afterMs, Callback callback)
{
    const std::weak_ptr weakOperation = operation;
    const auto id = state->scheduler.scheduleAfter(
        afterMs, [weakOperation, callback = std::move(callback)]() mutable {
            const auto locked = weakOperation.lock();
            if (!locked || !locked->active)
            {
                return;
            }
            callback(locked);
        });
    operation->taskIds.push_back(id);
}

}  // namespace

RumbleFixtureRequestHandle::RumbleFixtureRequestHandle(
    std::weak_ptr<detail::RumbleFixtureTransportState> transport,
    std::weak_ptr<detail::RumbleFixtureOperation> operation)
    : transport_(std::move(transport))
    , operation_(std::move(operation))
{
}

RumbleFixtureRequestHandle::RumbleFixtureRequestHandle(
    RumbleFixtureRequestHandle &&other) noexcept
    : transport_(std::move(other.transport_))
    , operation_(std::move(other.operation_))
{
}

RumbleFixtureRequestHandle &RumbleFixtureRequestHandle::operator=(
    RumbleFixtureRequestHandle &&other) noexcept
{
    if (this != &other)
    {
        this->cancel();
        this->transport_ = std::move(other.transport_);
        this->operation_ = std::move(other.operation_);
    }
    return *this;
}

RumbleFixtureRequestHandle::~RumbleFixtureRequestHandle()
{
    this->cancel();
}

void RumbleFixtureRequestHandle::cancel() noexcept
{
    const auto transport = this->transport_.lock();
    const auto operation = this->operation_.lock();
    if (transport && operation)
    {
        transport->cancel(operation);
    }
    this->transport_.reset();
    this->operation_.reset();
}

bool RumbleFixtureRequestHandle::active() const noexcept
{
    const auto operation = this->operation_.lock();
    return operation && operation->active;
}

RumbleFixtureTransport::RumbleFixtureTransport(
    ManualScheduler &scheduler, RumbleFixtureScript script)
    : state_(std::make_shared<detail::RumbleFixtureTransportState>(
          scheduler, std::move(script)))
{
}

RumbleFixtureTransport::~RumbleFixtureTransport()
{
    this->cancelAll();
}

RumbleFixtureRequestHandle RumbleFixtureTransport::start(
    RumbleFixtureRequest request, RumbleFixtureCallbacks callbacks)
{
    if (this->state_->nextExchange >= this->state_->script.exchanges.size())
    {
        throw std::logic_error("fixture script '" + this->state_->script.name +
                               "' has no remaining exchanges");
    }

    const auto &exchange =
        this->state_->script.exchanges[this->state_->nextExchange];
    validateRequest(request, exchange.expectedRequest, exchange.label);
    ++this->state_->nextExchange;

    auto operation = std::make_shared<detail::RumbleFixtureOperation>();
    operation->callbacks = std::move(callbacks);
    this->state_->operations.push_back(operation);

    std::int64_t offsetMs = exchange.headAfterMs;
    scheduleOperationTask(
        this->state_, operation, offsetMs,
        [head = exchange.response](const auto &locked) {
            const auto callback = locked->callbacks.onHead;
            if (callback)
            {
                callback(head);
            }
        });

    for (const auto &chunk : exchange.chunks)
    {
        if (chunk.afterMs >
            std::numeric_limits<std::int64_t>::max() - offsetMs)
        {
            this->state_->cancel(operation);
            throw std::overflow_error("fixture exchange delay overflow");
        }
        offsetMs += chunk.afterMs;
        scheduleOperationTask(
            this->state_, operation, offsetMs,
            [bytes = chunk.bytes](const auto &locked) {
                const auto callback = locked->callbacks.onBodyChunk;
                if (callback)
                {
                    callback(bytes);
                }
            });
    }

    if (exchange.terminalAfterMs >
        std::numeric_limits<std::int64_t>::max() - offsetMs)
    {
        this->state_->cancel(operation);
        throw std::overflow_error("fixture terminal delay overflow");
    }
    offsetMs += exchange.terminalAfterMs;

    const std::weak_ptr weakState = this->state_;
    scheduleOperationTask(
        this->state_, operation, offsetMs,
        [weakState, terminal = exchange.terminal,
         reason = exchange.terminalReason](const auto &locked) {
            const auto state = weakState.lock();
            if (!state)
            {
                return;
            }

            auto callbacks = std::move(locked->callbacks);
            locked->active = false;
            locked->taskIds.clear();
            state->retire(locked);

            if (terminal == RumbleFixtureTerminal::Disconnect)
            {
                if (callbacks.onDisconnect)
                {
                    callbacks.onDisconnect(reason);
                }
            }
            else if (callbacks.onComplete)
            {
                callbacks.onComplete();
            }
        });

    return {this->state_, operation};
}

void RumbleFixtureTransport::cancelAll() noexcept
{
    if (this->state_)
    {
        this->state_->cancelAll();
    }
}

std::size_t RumbleFixtureTransport::activeRequestCount() const noexcept
{
    return this->state_ ? this->state_->operations.size() : 0;
}

std::size_t RumbleFixtureTransport::remainingExchangeCount() const noexcept
{
    if (!this->state_)
    {
        return 0;
    }
    return this->state_->script.exchanges.size() -
           this->state_->nextExchange;
}

}  // namespace chatterino::test
