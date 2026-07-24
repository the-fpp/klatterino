// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleQtTransport.hpp"

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace chatterino::rumble {
namespace {

constexpr qsizetype MAX_REPLY_READ_BUFFER_BYTES = 64 * 1024;

bool eventStreamMediaTypeMatches(const QByteArray &value)
{
    const auto parts = value.trimmed().toLower().split(';');
    if (parts.empty() ||
        parts.front().trimmed() != QByteArrayLiteral("text/event-stream"))
    {
        return false;
    }
    if (parts.size() == 1)
    {
        return true;
    }
    if (parts.size() > 3)
    {
        return false;
    }
    return std::ranges::all_of(parts.cbegin() + 1, parts.cend(),
                               [](const QByteArray &parameter) {
                                   return parameter.trimmed() ==
                                          QByteArrayLiteral("charset=utf-8");
                               });
}

bool responseMediaMatches(QNetworkReply &reply, ExpectedMediaType expected)
{
    const auto status =
        reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 204)
    {
        return true;
    }

    const auto value =
        reply.header(QNetworkRequest::ContentTypeHeader).toByteArray();
    if (expected == ExpectedMediaType::EventStream)
    {
        return eventStreamMediaTypeMatches(value);
    }
    const auto normalized = value.trimmed().toLower();
    const auto semicolon = normalized.indexOf(';');
    const auto essence =
        (semicolon < 0 ? normalized : normalized.left(semicolon)).trimmed();
    if (expected == ExpectedMediaType::Html)
    {
        return essence == QByteArrayLiteral("text/html");
    }
    return essence == QByteArrayLiteral("application/json") ||
           (essence.startsWith("application/") &&
            essence.endsWith("+json"));
}

std::optional<std::vector<Header>> normalizedResponseHeaders(
    QNetworkReply &reply)
{
    std::vector<Header> result;
    for (const auto &[name, value] : reply.rawHeaderPairs())
    {
        if (name.isEmpty() || name.contains('\0') || name.contains('\r') ||
            name.contains('\n') || value.contains('\0') || value.contains('\r'))
        {
            return std::nullopt;
        }

        if (name.compare(QByteArrayLiteral("set-cookie"),
                         Qt::CaseInsensitive) == 0)
        {
            // Qt folds repeated Set-Cookie response fields into one raw
            // value separated by LF. Expand that internal representation so
            // the transport never exposes control bytes to the API layer.
            for (auto cookie : value.split('\n'))
            {
                if (cookie.isEmpty())
                {
                    return std::nullopt;
                }
                result.push_back({name, std::move(cookie)});
            }
            continue;
        }
        if (value.contains('\n'))
        {
            return std::nullopt;
        }
        result.push_back({name, value});
    }
    return result;
}

bool responseHeadersWithinLimits(const std::vector<Header> &headers,
                                 const TransportRequest &request)
{
    if (static_cast<int>(headers.size()) > request.maxHeaders)
    {
        return false;
    }
    qsizetype bytes = 0;
    for (const auto &header : headers)
    {
        const auto &name = header.name;
        const auto &value = header.value;
        if (name.isEmpty() || name.contains('\0') || name.contains('\r') ||
            name.contains('\n') || value.contains('\0') ||
            value.contains('\r') || value.contains('\n') ||
            bytes > request.maxHeaderBytes ||
            name.size() > request.maxHeaderBytes - bytes)
        {
            return false;
        }
        const auto remaining = request.maxHeaderBytes - bytes - name.size();
        if (remaining < 4 || value.size() > remaining - 4)
        {
            return false;
        }
        bytes += name.size() + value.size() + 4;
    }
    return bytes <= request.maxHeaderBytes;
}

ResponseHead responseHead(QNetworkReply &reply, std::vector<Header> headers)
{
    ResponseHead result;
    result.status =
        reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.headers = std::move(headers);
    return result;
}

bool isAllowedHeader(const Header &header, ExpectedMediaType media)
{
    const auto trimmedName = header.name.trimmed();
    const auto name = trimmedName.toLower();
    if (trimmedName.size() != header.name.size() || header.name.contains('\r') ||
        header.name.contains('\n') || header.name.contains('\0') ||
        header.value.contains('\r') || header.value.contains('\n') ||
        header.value.contains('\0'))
    {
        return false;
    }
    if (name == QByteArrayLiteral("accept"))
    {
        if (media == ExpectedMediaType::Html)
        {
            return header.value == QByteArrayLiteral("text/html");
        }
        if (media == ExpectedMediaType::Json)
        {
            return header.value == QByteArrayLiteral("application/json");
        }
        return header.value == QByteArrayLiteral("text/event-stream");
    }
    if (name == QByteArrayLiteral("user-agent"))
    {
        return header.value == QByteArrayLiteral("chatterino-rumble/1");
    }
    if (media != ExpectedMediaType::EventStream)
    {
        return false;
    }
    if (name == QByteArrayLiteral("cache-control"))
    {
        return header.value == QByteArrayLiteral("no-cache");
    }
    if (name == QByteArrayLiteral("origin"))
    {
        return header.value == QByteArrayLiteral("https://rumble.com");
    }
    if (name == QByteArrayLiteral("referer"))
    {
        return header.value == QByteArrayLiteral("https://rumble.com/");
    }
    return false;
}

int headerCount(const std::vector<Header> &headers, const QByteArray &name)
{
    return static_cast<int>(
        std::ranges::count_if(headers, [&name](const Header &header) {
            return header.name.trimmed().compare(name, Qt::CaseInsensitive) ==
                   0;
        }));
}

bool hasRequiredHeaders(const std::vector<Header> &headers,
                        ExpectedMediaType media)
{
    if (headerCount(headers, QByteArrayLiteral("accept")) != 1 ||
        headerCount(headers, QByteArrayLiteral("user-agent")) != 1)
    {
        return false;
    }
    if (media != ExpectedMediaType::EventStream)
    {
        return headers.size() == 2;
    }
    return headers.size() == 5 &&
           headerCount(headers, QByteArrayLiteral("cache-control")) == 1 &&
           headerCount(headers, QByteArrayLiteral("origin")) == 1 &&
           headerCount(headers, QByteArrayLiteral("referer")) == 1;
}

bool sameAllowedRedirect(const QUrl &from, const QUrl &to,
                         ExpectedMediaType media)
{
    if (from.scheme().compare(to.scheme(), Qt::CaseInsensitive) != 0 ||
        from.host().compare(to.host(), Qt::CaseInsensitive) != 0 ||
        !RumbleQtTransport::isAllowedUrl(from, media) ||
        !RumbleQtTransport::isAllowedUrl(to, media))
    {
        return false;
    }

    // Every supported endpoint encodes its stable resource selector in the
    // canonical path/query. A same-origin redirect may not change that
    // selector (or switch channel/video endpoint families).
    return from.path(QUrl::FullyEncoded) == to.path(QUrl::FullyEncoded) &&
           from.query(QUrl::FullyEncoded) ==
               to.query(QUrl::FullyEncoded) &&
           from.hasQuery() == to.hasQuery() &&
           from.fragment(QUrl::FullyEncoded) ==
               to.fragment(QUrl::FullyEncoded) &&
           from.hasFragment() == to.hasFragment();
}

std::optional<TransportFailure> replyFailure(QNetworkReply &reply,
                                             int status)
{
    const auto error = reply.error();
    if (error == QNetworkReply::OperationCanceledError)
    {
        return TransportFailure::Cancelled;
    }

    // Qt reports HTTP 4xx/5xx through QNetworkReply::error(). Preserve the
    // response so the API can classify it by status instead of flattening it
    // into a transport failure.
    if (status >= 400 && status <= 599)
    {
        return std::nullopt;
    }
    if (error == QNetworkReply::TimeoutError)
    {
        return TransportFailure::Timeout;
    }
    if (error != QNetworkReply::NoError || status == 0)
    {
        // RemoteHostClosedError is the common premature-close/truncation
        // signal. Other non-HTTP reply errors are network failures too.
        return TransportFailure::Network;
    }
    return std::nullopt;
}

}  // namespace

namespace {

struct QtOperation {
    std::weak_ptr<RumbleQtTransport::State> state;
    TransportRequest request;
    TransportCallbacks callbacks;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> timer;
    int redirectCount = 0;
    std::uint64_t replyGeneration = 0;
    qsizetype receivedBytes = 0;
    qsizetype pendingDeliveryBytes = 0;
    int pendingDeliveryChunks = 0;
    bool headQueued = false;
    bool active = true;
    bool terminalQueued = false;
    bool bodyDeliveryPending = false;
    bool replyFinished = false;
};

class QtHandle final : public TransportHandle
{
public:
    QtHandle(std::weak_ptr<RumbleQtTransport::State> state,
             std::weak_ptr<QtOperation> operation)
        : state_(std::move(state))
        , operation_(std::move(operation))
    {
    }

    ~QtHandle() override
    {
        this->cancel();
    }

    void cancel() noexcept override;
    [[nodiscard]] bool active() const noexcept override
    {
        const auto operation = this->operation_.lock();
        return operation && operation->active;
    }

private:
    std::weak_ptr<RumbleQtTransport::State> state_;
    std::weak_ptr<QtOperation> operation_;
};

}  // namespace

struct RumbleQtTransport::State
    : public std::enable_shared_from_this<RumbleQtTransport::State> {
    QPointer<QNetworkAccessManager> manager;
    QPointer<QObject> context;
    std::vector<std::shared_ptr<QtOperation>> operations;
    bool shuttingDown = false;

    void retire(const std::shared_ptr<QtOperation> &operation)
    {
        std::erase(this->operations, operation);
    }

    void cancel(const std::shared_ptr<QtOperation> &operation) noexcept
    {
        if (!operation || !operation->active)
        {
            return;
        }
        operation->active = false;
        operation->terminalQueued = true;
        if (operation->timer)
        {
            operation->timer->stop();
            operation->timer->deleteLater();
            operation->timer = nullptr;
        }
        if (operation->reply)
        {
            operation->reply->disconnect(this->context);
            operation->reply->abort();
            operation->reply->deleteLater();
            operation->reply = nullptr;
        }
        operation->callbacks = {};
        this->retire(operation);
    }

    void cancelAll() noexcept
    {
        const auto copy = this->operations;
        for (const auto &operation : copy)
        {
            this->cancel(operation);
        }
    }
};

namespace {

void QtHandle::cancel() noexcept
{
    const auto state = this->state_.lock();
    const auto operation = this->operation_.lock();
    if (state && operation)
    {
        state->cancel(operation);
    }
    this->state_.reset();
    this->operation_.reset();
}

template <typename Callback>
void queueForOperation(const std::shared_ptr<RumbleQtTransport::State> &state,
                       const std::shared_ptr<QtOperation> &operation,
                       Callback callback)
{
    if (!state->context)
    {
        state->cancel(operation);
        return;
    }
    const std::weak_ptr weakState = state;
    const std::weak_ptr weakOperation = operation;
    QMetaObject::invokeMethod(
        state->context,
        [weakState, weakOperation,
         callback = std::move(callback)]() mutable {
            const auto lockedState = weakState.lock();
            const auto lockedOperation = weakOperation.lock();
            if (!lockedState || !lockedOperation ||
                !lockedOperation->active)
            {
                return;
            }
            callback(lockedState, lockedOperation);
        },
        Qt::QueuedConnection);
}

void finishFailure(const std::shared_ptr<RumbleQtTransport::State> &state,
                   const std::shared_ptr<QtOperation> &operation,
                   TransportFailure failure)
{
    if (!operation->active || operation->terminalQueued)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->timer)
    {
        operation->timer->stop();
    }
    if (operation->reply)
    {
        operation->reply->abort();
    }

    queueForOperation(
        state, operation,
        [failure](const auto &lockedState, const auto &lockedOperation) {
            lockedOperation->active = false;
            if (lockedOperation->timer)
            {
                lockedOperation->timer->deleteLater();
                lockedOperation->timer = nullptr;
            }
            if (lockedOperation->reply)
            {
                lockedOperation->reply->deleteLater();
                lockedOperation->reply = nullptr;
            }
            auto callbacks = std::move(lockedOperation->callbacks);
            lockedState->retire(lockedOperation);
            if (callbacks.onFailure)
            {
                callbacks.onFailure(failure);
            }
        });
}

bool discardRedirectBody(
    const std::shared_ptr<RumbleQtTransport::State> &state,
    const std::shared_ptr<QtOperation> &operation)
{
    if (!operation->reply)
    {
        finishFailure(state, operation, TransportFailure::Network);
        return false;
    }
    const auto headers = normalizedResponseHeaders(*operation->reply);
    if (!headers || !responseHeadersWithinLimits(*headers, operation->request))
    {
        finishFailure(state, operation, TransportFailure::HeaderLimit);
        return false;
    }

    const auto bytes = operation->reply->readAll();
    if (bytes.size() >
        operation->request.maxBodyBytes - operation->receivedBytes)
    {
        finishFailure(state, operation, TransportFailure::BodyLimit);
        return false;
    }
    operation->receivedBytes += bytes.size();
    return true;
}

void drainReply(const std::shared_ptr<RumbleQtTransport::State> &state,
                const std::shared_ptr<QtOperation> &operation);
void finishComplete(const std::shared_ptr<RumbleQtTransport::State> &state,
                    const std::shared_ptr<QtOperation> &operation);
bool queueHead(const std::shared_ptr<RumbleQtTransport::State> &state,
               const std::shared_ptr<QtOperation> &operation);

void finishDrainedReply(
    const std::shared_ptr<RumbleQtTransport::State> &state,
    const std::shared_ptr<QtOperation> &operation)
{
    if (!operation->reply || !operation->active ||
        operation->terminalQueued || !operation->replyFinished ||
        operation->bodyDeliveryPending ||
        operation->reply->bytesAvailable() > 0)
    {
        return;
    }

    const auto status =
        operation->reply
            ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
            .toInt();
    if (const auto failure = replyFailure(*operation->reply, status))
    {
        finishFailure(state, operation, *failure);
        return;
    }
    if (!operation->headQueued && !queueHead(state, operation))
    {
        return;
    }
    finishComplete(state, operation);
}

void finishComplete(const std::shared_ptr<RumbleQtTransport::State> &state,
                    const std::shared_ptr<QtOperation> &operation)
{
    if (!operation->active || operation->terminalQueued)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->timer)
    {
        operation->timer->stop();
    }

    queueForOperation(
        state, operation,
        [](const auto &lockedState, const auto &lockedOperation) {
            lockedOperation->active = false;
            if (lockedOperation->timer)
            {
                lockedOperation->timer->deleteLater();
                lockedOperation->timer = nullptr;
            }
            if (lockedOperation->reply)
            {
                lockedOperation->reply->deleteLater();
                lockedOperation->reply = nullptr;
            }
            auto callbacks = std::move(lockedOperation->callbacks);
            lockedState->retire(lockedOperation);
            if (callbacks.onComplete)
            {
                callbacks.onComplete();
            }
        });
}

bool queueHead(const std::shared_ptr<RumbleQtTransport::State> &state,
               const std::shared_ptr<QtOperation> &operation)
{
    if (operation->headQueued)
    {
        return true;
    }
    if (!operation->reply)
    {
        finishFailure(state, operation, TransportFailure::Network);
        return false;
    }

    const auto redirect = operation->reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid() && !redirect.toUrl().isEmpty())
    {
        // Redirect bodies are never exposed to the API operation.
        return false;
    }
    const auto status =
        operation->reply
            ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
            .toInt();
    if (status == 0)
    {
        return false;
    }
    auto headers = normalizedResponseHeaders(*operation->reply);
    if (!headers || !responseHeadersWithinLimits(*headers, operation->request))
    {
        finishFailure(state, operation, TransportFailure::HeaderLimit);
        return false;
    }
    if (status >= 200 && status < 300 &&
        !responseMediaMatches(*operation->reply,
                              operation->request.expectedMediaType))
    {
        finishFailure(state, operation, TransportFailure::InvalidMediaType);
        return false;
    }

    operation->headQueued = true;
    if (operation->request.deadlineScope == DeadlineScope::UntilFinalHead &&
        operation->timer)
    {
        operation->timer->stop();
        operation->timer->deleteLater();
        operation->timer = nullptr;
    }
    auto head = responseHead(*operation->reply, std::move(*headers));
    queueForOperation(
        state, operation,
        [head = std::move(head)](const auto &, const auto &lockedOperation) {
            const auto callback = lockedOperation->callbacks.onHead;
            if (callback)
            {
                callback(head);
            }
        });
    return true;
}

void drainReply(const std::shared_ptr<RumbleQtTransport::State> &state,
                const std::shared_ptr<QtOperation> &operation)
{
    if (!operation->reply || !operation->active ||
        operation->terminalQueued || operation->bodyDeliveryPending)
    {
        return;
    }
    const auto redirect = operation->reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid() && !redirect.toUrl().isEmpty())
    {
        // Redirect payloads are counted and discarded rather than forwarded.
        // This keeps every response in the chain under the same bounds.
        discardRedirectBody(state, operation);
        return;
    }
    if (!queueHead(state, operation))
    {
        return;
    }

    const auto pendingScope =
        operation->request.bodyLimitScope == BodyLimitScope::PendingDelivery;
    const auto accounted = pendingScope ? operation->pendingDeliveryBytes
                                        : operation->receivedBytes;
    const auto remaining = operation->request.maxBodyBytes - accounted;
    if (remaining <= 0 ||
        (pendingScope && operation->pendingDeliveryChunks >=
                             operation->request.maxPendingBodyChunks))
    {
        if (pendingScope)
        {
            // Pending-delivery limits are a backpressure window, not a limit
            // on the lifetime size of a persistent response. Leave unread
            // bytes in QNetworkReply until the queued callback releases room.
            return;
        }
        if (operation->reply->bytesAvailable() > 0)
        {
            finishFailure(state, operation, TransportFailure::BodyLimit);
        }
        else
        {
            finishDrainedReply(state, operation);
        }
        return;
    }

    const auto maximumRead =
        std::min<qsizetype>(remaining, MAX_REPLY_READ_BUFFER_BYTES);
    auto bytes = operation->reply->read(static_cast<qint64>(maximumRead));
    if (bytes.isEmpty())
    {
        finishDrainedReply(state, operation);
        return;
    }
    if (pendingScope)
    {
        operation->pendingDeliveryBytes += bytes.size();
        ++operation->pendingDeliveryChunks;
    }
    else
    {
        operation->receivedBytes += bytes.size();
    }
    operation->bodyDeliveryPending = true;
    queueForOperation(
        state, operation,
        [bytes = std::move(bytes)](const auto &lockedState,
                                   const auto &lockedOperation) {
            if (lockedOperation->request.bodyLimitScope ==
                BodyLimitScope::PendingDelivery)
            {
                lockedOperation->pendingDeliveryBytes -= bytes.size();
                --lockedOperation->pendingDeliveryChunks;
            }
            lockedOperation->bodyDeliveryPending = false;
            const auto callback = lockedOperation->callbacks.onBodyChunk;
            if (callback)
            {
                callback(bytes);
            }
            if (lockedOperation->active &&
                !lockedOperation->terminalQueued)
            {
                drainReply(lockedState, lockedOperation);
            }
        });
}

void beginReply(const std::shared_ptr<RumbleQtTransport::State> &state,
                const std::shared_ptr<QtOperation> &operation,
                const QUrl &url);

void replyFinished(const std::shared_ptr<RumbleQtTransport::State> &state,
                   const std::shared_ptr<QtOperation> &operation)
{
    if (!operation->reply || !operation->active ||
        operation->terminalQueued)
    {
        return;
    }

    const auto currentUrl = operation->reply->url();
    const auto status = operation->reply
                            ->attribute(
                                QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt();
    const auto redirectValue = operation->reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute);
    if (redirectValue.isValid() && !redirectValue.toUrl().isEmpty())
    {
        if (!discardRedirectBody(state, operation))
        {
            return;
        }
        if (const auto failure = replyFailure(*operation->reply, status))
        {
            finishFailure(state, operation, *failure);
            return;
        }
        const auto target = currentUrl.resolved(redirectValue.toUrl());
        if (status < 300 || status >= 400 ||
            operation->redirectCount >= operation->request.maxRedirects ||
            !sameAllowedRedirect(currentUrl, target,
                                 operation->request.expectedMediaType))
        {
            finishFailure(state, operation,
                          TransportFailure::RedirectRejected);
            return;
        }

        ++operation->redirectCount;
        operation->reply->deleteLater();
        operation->reply = nullptr;
        operation->headQueued = false;
        operation->receivedBytes = 0;
        beginReply(state, operation, target);
        return;
    }

    // A finished QNetworkReply may still own more decoded bytes than the
    // bounded delivery window. Drain one bounded chunk at a time and complete
    // only after its final queued body callback has run.
    operation->replyFinished = true;
    drainReply(state, operation);
    finishDrainedReply(state, operation);
}

void beginReply(const std::shared_ptr<RumbleQtTransport::State> &state,
                const std::shared_ptr<QtOperation> &operation,
                const QUrl &url)
{
    if (!state->manager || !state->context || !operation->active ||
        operation->terminalQueued)
    {
        state->cancel(operation);
        return;
    }
    // Invalidate any detached redirect reply before this attempt can fail.
    // Its deferred destruction must never retire a later operation state.
    const auto replyGeneration = ++operation->replyGeneration;

    auto redirected = operation->request;
    redirected.url = url;
    const auto request = RumbleQtTransport::prepareRequest(redirected);
    if (!request)
    {
        finishFailure(state, operation, TransportFailure::RedirectRejected);
        return;
    }

    auto *reply = state->manager->get(*request);
    if (!reply)
    {
        // Defensive fail-closed handling for non-conforming wrappers. Qt's
        // public QNetworkAccessManager request API promises a reply object.
        finishFailure(state, operation, TransportFailure::Network);
        return;
    }
    if (!state->context || reply->thread() != state->context->thread() ||
        reply->thread() != QThread::currentThread())
    {
        // A custom manager can violate QNetworkAccessManager's same-thread
        // reply contract. Do not touch that reply from the transport thread;
        // queue its cleanup on its own thread and fail the operation closed.
        const QPointer<QNetworkReply> guardedReply(reply);
        QMetaObject::invokeMethod(
            reply,
            [guardedReply] {
                if (!guardedReply)
                {
                    return;
                }
                guardedReply->abort();
                if (guardedReply)
                {
                    guardedReply->deleteLater();
                }
            },
            Qt::QueuedConnection);
        finishFailure(state, operation, TransportFailure::Network);
        return;
    }
    operation->reply = reply;
    operation->replyFinished = false;
    const auto bodyDetectionLimit =
        operation->request.maxBodyBytes ==
                std::numeric_limits<qsizetype>::max()
            ? operation->request.maxBodyBytes
            : operation->request.maxBodyBytes + 1;
    // Keep finite replies moving through the API in bounded pieces. Page
    // resolution can then cancel after its authoritative prefix instead of
    // buffering Rumble's unrelated multi-megabyte inline application. Tiny
    // test/consumer limits retain one extra byte for exact overflow detection.
    const auto readBufferLimit =
        std::min(bodyDetectionLimit, MAX_REPLY_READ_BUFFER_BYTES);
    reply->setReadBufferSize(static_cast<qint64>(readBufferLimit));

    const std::weak_ptr weakState = state;
    const std::weak_ptr weakOperation = operation;
    QObject::connect(
        reply, &QObject::destroyed, state->context,
        [weakState, weakOperation, replyGeneration] {
            const auto lockedState = weakState.lock();
            const auto lockedOperation = weakOperation.lock();
            if (!lockedState || !lockedOperation || !lockedOperation->active ||
                lockedOperation->replyGeneration != replyGeneration)
            {
                return;
            }
            lockedOperation->active = false;
            lockedOperation->terminalQueued = true;
            lockedOperation->reply = nullptr;
            if (lockedOperation->timer)
            {
                lockedOperation->timer->stop();
                lockedOperation->timer->deleteLater();
                lockedOperation->timer = nullptr;
            }
            lockedOperation->callbacks = {};
            lockedState->retire(lockedOperation);
        });
    QObject::connect(
        reply, &QNetworkReply::metaDataChanged, state->context,
        [weakState, weakOperation] {
            const auto lockedState = weakState.lock();
            const auto lockedOperation = weakOperation.lock();
            if (lockedState && lockedOperation &&
                lockedOperation->active &&
                !lockedOperation->terminalQueued)
            {
                queueHead(lockedState, lockedOperation);
            }
        });
    QObject::connect(
        reply, &QIODevice::readyRead, state->context,
        [weakState, weakOperation] {
            const auto lockedState = weakState.lock();
            const auto lockedOperation = weakOperation.lock();
            if (lockedState && lockedOperation)
            {
                drainReply(lockedState, lockedOperation);
            }
        });
    QObject::connect(
        reply, &QNetworkReply::finished, state->context,
        [weakState, weakOperation] {
            const auto lockedState = weakState.lock();
            const auto lockedOperation = weakOperation.lock();
            if (lockedState && lockedOperation)
            {
                replyFinished(lockedState, lockedOperation);
            }
        });
}

}  // namespace

RumbleQtTransport::RumbleQtTransport(QObject *owner)
    : QObject(owner)
    , state_(std::make_shared<State>())
{
    this->state_->context = this;
    auto *manager = new QNetworkAccessManager(this);
    this->state_->manager = manager;

    const std::weak_ptr weak = this->state_;
    QObject::connect(manager, &QObject::destroyed, this, [weak] {
        if (const auto state = weak.lock())
        {
            state->manager = nullptr;
            state->cancelAll();
        }
    });
}

RumbleQtTransport::RumbleQtTransport(QNetworkAccessManager &manager,
                                     QObject *owner)
    : QObject(owner)
    , state_(std::make_shared<State>())
{
    this->state_->context = this;
    this->state_->manager = &manager;

    const std::weak_ptr weak = this->state_;
    QObject::connect(&manager, &QObject::destroyed, this, [weak] {
        if (const auto state = weak.lock())
        {
            state->manager = nullptr;
            state->cancelAll();
        }
    });
}

RumbleQtTransport::~RumbleQtTransport()
{
    this->state_->shuttingDown = true;
    this->state_->cancelAll();
    this->state_->context = nullptr;
}

bool RumbleQtTransport::isAllowedUrl(const QUrl &url,
                                     ExpectedMediaType mediaType)
{
    const auto authority = url.authority(QUrl::FullyEncoded);
    if (!url.isValid() ||
        url.scheme().compare(QStringLiteral("https"),
                             Qt::CaseInsensitive) != 0 ||
        !url.userInfo().isEmpty() || authority.contains(u'@') ||
        authority.contains(u':') || url.port(-1) != -1 ||
        url.hasFragment())
    {
        return false;
    }

    auto encodedPath = url.path(QUrl::FullyEncoded);
    const auto lowerPath = encodedPath.toLower();
    if (lowerPath.contains(QStringLiteral("%2f")) ||
        lowerPath.contains(QStringLiteral("%5c")) ||
        lowerPath.contains(QStringLiteral("%00")))
    {
        return false;
    }

    if (mediaType == ExpectedMediaType::EventStream)
    {
        if (url.host().compare(QStringLiteral("web7.rumble.com"),
                               Qt::CaseInsensitive) != 0 ||
            url.hasQuery())
        {
            return false;
        }
        static const QRegularExpression ssePath(
            QStringLiteral("^/chat/api/chat/[1-9][0-9]{0,127}/stream$"));
        return ssePath.match(encodedPath).hasMatch();
    }

    if (url.host().compare(QStringLiteral("rumble.com"),
                           Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    if (mediaType == ExpectedMediaType::Html)
    {
        if (url.hasQuery())
        {
            return false;
        }
        static const QRegularExpression channelPath(
            QStringLiteral("^/(?:c|user)/[A-Za-z0-9][A-Za-z0-9_-]{0,79}/live/$"));
        static const QRegularExpression videoPath(
            QStringLiteral(
                "^/v[a-z0-9]{1,127}(?:-[A-Za-z0-9._~%+-]*)?\\.html$"));
        return channelPath.match(encodedPath).hasMatch() ||
               videoPath.match(encodedPath).hasMatch();
    }

    if (encodedPath == QStringLiteral("/service.php"))
    {
        QUrlQuery query(url);
        const auto items = query.queryItems(QUrl::FullyDecoded);
        if (items.size() != 2)
        {
            return false;
        }
        QString name;
        QString chatId;
        for (const auto &[key, value] : items)
        {
            if (key == QStringLiteral("name") && name.isEmpty())
            {
                name = value;
            }
            else if (key == QStringLiteral("chat_id") && chatId.isEmpty())
            {
                chatId = value;
            }
            else
            {
                return false;
            }
        }
        static const QRegularExpression streamId(
            QStringLiteral("^[1-9][0-9]{0,127}$"));
        return name == QStringLiteral("emote.list") &&
               streamId.match(chatId).hasMatch() &&
               url.query(QUrl::FullyEncoded) ==
                   QStringLiteral("name=emote.list&chat_id=") + chatId;
    }

    if (encodedPath != QStringLiteral("/embedJS/u3/"))
    {
        return false;
    }
    QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    if (items.size() != 3)
    {
        return false;
    }
    QString request;
    QString version;
    QString video;
    for (const auto &[name, value] : items)
    {
        if (name == QStringLiteral("request") && request.isEmpty())
        {
            request = value;
        }
        else if (name == QStringLiteral("ver") && version.isEmpty())
        {
            version = value;
        }
        else if (name == QStringLiteral("v") && video.isEmpty())
        {
            video = value;
        }
        else
        {
            return false;
        }
    }
    static const QRegularExpression embedId(
        QStringLiteral("^v[a-z0-9]{1,127}$"));
    if (request != QStringLiteral("video") ||
        version != QStringLiteral("2") ||
        !embedId.match(video).hasMatch())
    {
        return false;
    }
    return url.query(QUrl::FullyEncoded) ==
           QStringLiteral("request=video&ver=2&v=") + video;
}

std::optional<QNetworkRequest> RumbleQtTransport::prepareRequest(
    const TransportRequest &request)
{
    const bool finiteScopes =
        request.deadlineScope == DeadlineScope::WholeResponse &&
        request.bodyLimitScope == BodyLimitScope::Cumulative;
    const bool persistentScopes =
        request.deadlineScope == DeadlineScope::UntilFinalHead &&
        request.bodyLimitScope == BodyLimitScope::PendingDelivery;
    const bool scopesAllowed =
        finiteScopes ||
        (request.expectedMediaType == ExpectedMediaType::EventStream &&
         persistentScopes);
    if (!isAllowedUrl(request.url, request.expectedMediaType) ||
        request.maxBodyBytes <= 0 || request.maxHeaders <= 0 ||
        request.maxHeaderBytes <= 0 || request.timeoutMs <= 0 ||
        request.maxRedirects < 0 || request.maxRedirects > 8 ||
        request.maxPendingBodyChunks <= 0 || !scopesAllowed ||
        !hasRequiredHeaders(request.headers, request.expectedMediaType))
    {
        return std::nullopt;
    }

    std::vector<QByteArray> seen;
    for (const auto &header : request.headers)
    {
        const auto normalized = header.name.trimmed().toLower();
        if (normalized.isEmpty() || std::ranges::find(seen, normalized) !=
                                        seen.end() ||
            !isAllowedHeader(header, request.expectedMediaType))
        {
            return std::nullopt;
        }
        seen.push_back(normalized);
    }

    QNetworkRequest result(request.url);
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        QNetworkRequest::ManualRedirectPolicy);
    result.setMaximumRedirectsAllowed(request.maxRedirects);
    result.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                        QNetworkRequest::Manual);
    result.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                        QNetworkRequest::Manual);
    result.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                        QNetworkRequest::Manual);
    result.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                        QNetworkRequest::AlwaysNetwork);
    result.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    result.setAttribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute,
                        false);

    for (const auto &header : request.headers)
    {
        result.setRawHeader(header.name, header.value);
    }
    return result;
}

std::unique_ptr<TransportHandle> RumbleQtTransport::start(
    TransportRequest request, TransportCallbacks callbacks)
{
    auto operation = std::make_shared<QtOperation>();
    operation->state = this->state_;
    operation->request = std::move(request);
    operation->callbacks = std::move(callbacks);
    this->state_->operations.push_back(operation);

    auto handle =
        std::make_unique<QtHandle>(this->state_, operation);

    if (QThread::currentThread() != this->thread() ||
        !this->state_->manager ||
        this->state_->manager->thread() != this->thread())
    {
        finishFailure(this->state_, operation, TransportFailure::Network);
        return handle;
    }

    if (!prepareRequest(operation->request))
    {
        finishFailure(this->state_, operation,
                      TransportFailure::RedirectRejected);
        return handle;
    }

    operation->timer = new QTimer(this);
    operation->timer->setSingleShot(true);
    const std::weak_ptr weakState = this->state_;
    const std::weak_ptr weakOperation = operation;
    QObject::connect(
        operation->timer, &QTimer::timeout, this,
        [weakState, weakOperation] {
            const auto state = weakState.lock();
            const auto locked = weakOperation.lock();
            if (state && locked && locked->active &&
                !locked->terminalQueued)
            {
                finishFailure(state, locked, TransportFailure::Timeout);
            }
        });
    operation->timer->start(operation->request.timeoutMs);

    beginReply(this->state_, operation, operation->request.url);
    return handle;
}

}  // namespace chatterino::rumble
