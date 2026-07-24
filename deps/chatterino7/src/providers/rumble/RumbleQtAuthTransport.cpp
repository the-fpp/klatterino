// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleQtAuthTransport.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <utility>
#include <vector>

namespace chatterino::rumble {
namespace {

void wipe(QByteArray &value) noexcept
{
    volatile char *bytes = value.data();
    for (qsizetype i = 0; i < value.size(); ++i)
        bytes[i] = 0;
    value.clear();
}

struct Operation {
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> timer;
    AuthCallbacks callbacks;
    AuthOperation kind = AuthOperation::Probe;
    QByteArray body;
    bool active = true;
};

}  // namespace

struct RumbleQtAuthTransport::State
    : std::enable_shared_from_this<RumbleQtAuthTransport::State> {
    std::unique_ptr<QNetworkAccessManager> ownedManager;
    QPointer<QNetworkAccessManager> manager;
    QPointer<QObject> context;
    std::vector<std::shared_ptr<Operation>> operations;
    bool shuttingDown = false;

    void cancel(const std::shared_ptr<Operation> &operation) noexcept
    {
        if (!operation || !operation->active)
            return;
        operation->active = false;
        operation->callbacks = {};
        if (operation->timer)
        {
            operation->timer->stop();
            operation->timer->deleteLater();
        }
        if (operation->reply)
        {
            operation->reply->disconnect(context);
            operation->reply->abort();
            operation->reply->deleteLater();
        }
        std::erase(operations, operation);
    }
};

namespace {
class Handle final : public AuthHandle
{
public:
    Handle(std::weak_ptr<RumbleQtAuthTransport::State> state,
           std::weak_ptr<Operation> operation)
        : state_(std::move(state))
        , operation_(std::move(operation))
    {
    }
    ~Handle() override { cancel(); }
    void cancel() noexcept override
    {
        if (const auto state = state_.lock())
            state->cancel(operation_.lock());
        state_.reset();
        operation_.reset();
    }

private:
    std::weak_ptr<RumbleQtAuthTransport::State> state_;
    std::weak_ptr<Operation> operation_;
};

bool validStream(const QString &stream)
{
    return !stream.isEmpty() && stream.size() <= 32 &&
           std::ranges::all_of(stream, [](QChar ch) { return ch.isDigit(); });
}

bool validBearer(const QByteArray &bearer)
{
    return !bearer.isEmpty() && bearer.size() <= 4096 &&
           std::ranges::none_of(bearer, [](unsigned char ch) {
               return ch <= 0x20 || ch == 0x7f || ch == ';' || ch == ',';
           });
}

bool responseHeadersWithinLimits(QNetworkReply &reply)
{
    qsizetype headerBytes = 0;
    const auto headers = reply.rawHeaderPairs();
    for (const auto &[name, value] : headers)
        headerBytes += name.size() + value.size() + 4;
    return headers.size() <= 64 && headerBytes <= 16 * 1024;
}

qsizetype firstEventEnd(const QByteArray &body)
{
    auto end = body.indexOf("\n\n");
    const auto crlf = body.indexOf("\r\n\r\n");
    if (end < 0 || (crlf >= 0 && crlf < end))
    {
        end = crlf < 0 ? -1 : crlf + 2;
    }
    return end < 0 ? -1 : end + 2;
}
}  // namespace

RumbleQtAuthTransport::RumbleQtAuthTransport(QObject *owner)
    : QObject(owner)
    , state_(std::make_shared<State>())
{
    state_->context = this;
    state_->ownedManager = std::make_unique<QNetworkAccessManager>(this);
    state_->manager = state_->ownedManager.get();
}

RumbleQtAuthTransport::RumbleQtAuthTransport(QNetworkAccessManager &manager,
                                             QObject *owner)
    : QObject(owner)
    , state_(std::make_shared<State>())
{
    state_->context = this;
    state_->manager = &manager;
}

RumbleQtAuthTransport::~RumbleQtAuthTransport()
{
    state_->shuttingDown = true;
    const auto operations = state_->operations;
    for (const auto &operation : operations)
        state_->cancel(operation);
}

std::unique_ptr<AuthHandle> RumbleQtAuthTransport::start(
    AuthOperation kind, QString streamId, QString text, QByteArray bearer,
    QByteArray requestId, AuthCallbacks callbacks)
{
    const auto state = state_;
    const auto reject = [&] {
        wipe(bearer);
        return std::unique_ptr<AuthHandle>{};
    };
    if (!state || state->shuttingDown || !state->manager ||
        !validBearer(bearer) ||
        (kind == AuthOperation::Probe &&
         (!streamId.isEmpty() || !text.isEmpty() || !requestId.isEmpty())) ||
        (kind == AuthOperation::Eligibility &&
         (!validStream(streamId) || !text.isEmpty() ||
          !requestId.isEmpty())) ||
        (kind == AuthOperation::Send &&
         (!validStream(streamId) || text.isEmpty() ||
          text.size() > SessionController::ABSOLUTE_TEXT_LIMIT ||
          requestId.size() != 43)))
        return reject();

    QUrl url;
    QByteArray body;
    if (kind == AuthOperation::Probe)
    {
        url = QUrl(QStringLiteral("https://rumble.com/service.php"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("name"),
                           QStringLiteral("user.has_unread_notifications"));
        url.setQuery(query);
    }
    else if (kind == AuthOperation::Eligibility)
    {
        url = QUrl(
            QStringLiteral("https://web7.rumble.com/chat/api/chat/%1/stream")
                .arg(streamId));
    }
    else
    {
        url = QUrl(QStringLiteral("https://web7.rumble.com/chat/api/chat/%1/message")
                       .arg(streamId));
        body = QJsonDocument(QJsonObject{
                                 {QStringLiteral("data"),
                                  QJsonObject{
                                      {QStringLiteral("request_id"),
                                       QString::fromLatin1(requestId)},
                                      {QStringLiteral("message"),
                                       QJsonObject{{QStringLiteral("text"), text}}},
                                      {QStringLiteral("rant"), QJsonValue::Null},
                                      {QStringLiteral("channel_id"), QJsonValue::Null},
                                  }},
                             })
                   .toJson(QJsonDocument::Compact);
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setRawHeader(
        "Accept", kind == AuthOperation::Eligibility
                      ? QByteArrayLiteral("text/event-stream")
                      : QByteArrayLiteral("application/json, text/plain, */*"));
    request.setRawHeader("Cookie", QByteArrayLiteral("u_s=") + bearer);
    if (kind != AuthOperation::Eligibility)
        request.setRawHeader("Content-Type", kind == AuthOperation::Probe
                                                 ? QByteArrayLiteral("application/x-www-form-urlencoded")
                                                 : QByteArrayLiteral("application/json"));
    if (kind == AuthOperation::Send || kind == AuthOperation::Eligibility)
        request.setRawHeader("Origin", "https://rumble.com");
    if (kind == AuthOperation::Eligibility)
    {
        request.setRawHeader("Referer", "https://rumble.com/");
        request.setRawHeader("Cache-Control", "no-cache");
    }
    wipe(bearer);

    auto operation = std::make_shared<Operation>();
    operation->kind = kind;
    operation->callbacks = std::move(callbacks);
    operation->reply = kind != AuthOperation::Send
                           ? state->manager->get(request)
                           : state->manager->post(request, body);
    request.setRawHeader("Cookie", {});
    wipe(body);
    if (!operation->reply)
        return reject();
    operation->timer = new QTimer(this);
    operation->timer->setSingleShot(true);
    state->operations.push_back(operation);

    const std::weak_ptr weakState = state;
    const std::weak_ptr weakOperation = operation;
    QObject::connect(operation->timer, &QTimer::timeout, this,
                     [weakState, weakOperation] {
        const auto self = weakState.lock();
        const auto op = weakOperation.lock();
        if (!self || !op || !op->active)
            return;
        auto callback = std::move(op->callbacks.failed);
        op->callbacks = {};
        self->cancel(op);
        if (callback)
            callback(AuthFailure::Timeout);
    });
    if (kind == AuthOperation::Eligibility)
    {
        QObject::connect(operation->reply, &QIODevice::readyRead, this,
                         [weakState, weakOperation] {
            const auto self = weakState.lock();
            const auto op = weakOperation.lock();
            if (!self || !op || !op->active || !op->reply)
                return;
            constexpr qsizetype LIMIT = 1024 * 1024;
            const auto remaining = LIMIT + 1 - op->body.size();
            if (remaining > 0)
                op->body.append(op->reply->read(remaining));
            const auto end = firstEventEnd(op->body);
            if (op->body.size() <= LIMIT && end < 0)
                return;

            AuthCallbacks callbacks = std::move(op->callbacks);
            const auto reply = op->reply;
            const auto status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool redirect = reply->attribute(
                QNetworkRequest::RedirectionTargetAttribute).isValid();
            const bool headersOk = responseHeadersWithinLimits(*reply);
            AuthResponse response{
                .status = status,
                .contentType = reply->header(
                    QNetworkRequest::ContentTypeHeader).toByteArray(),
                .body = end >= 0 ? op->body.left(end) : QByteArray{},
            };
            const bool tooLarge = op->body.size() > LIMIT;
            op->callbacks = {};
            self->cancel(op);
            if (tooLarge || !headersOk)
            {
                if (callbacks.failed)
                    callbacks.failed(AuthFailure::ResponseLimit);
            }
            else if (redirect)
            {
                if (callbacks.failed)
                    callbacks.failed(AuthFailure::RedirectRejected);
            }
            else if (callbacks.complete)
            {
                callbacks.complete(std::move(response));
            }
        });
    }
    QObject::connect(operation->reply, &QNetworkReply::finished, this,
                     [weakState, weakOperation] {
        const auto self = weakState.lock();
        const auto op = weakOperation.lock();
        if (!self || !op || !op->active || !op->reply)
            return;
        const auto reply = op->reply;
        const auto status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const bool redirect = reply->attribute(
            QNetworkRequest::RedirectionTargetAttribute).isValid();
        AuthCallbacks callbacks = std::move(op->callbacks);
        const auto bodyLimit = op->kind == AuthOperation::Eligibility
                                   ? 1024 * 1024
                                   : 64 * 1024;
        if (op->kind == AuthOperation::Eligibility)
        {
            const auto remaining = bodyLimit + 1 - op->body.size();
            if (remaining > 0)
                op->body.append(reply->read(remaining));
        }
        AuthResponse response{
            .status = status,
            .contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray(),
            .body = op->kind == AuthOperation::Eligibility
                        ? std::move(op->body)
                        : reply->read(bodyLimit + 1),
        };
        const auto retry = reply->rawHeader("Retry-After");
        if (!retry.isEmpty())
            response.retryAfter = retry;
        const bool tooLarge = response.body.size() > bodyLimit ||
                              reply->bytesAvailable() > 0;
        const bool headersTooLarge = !responseHeadersWithinLimits(*reply);
        op->callbacks = {};
        self->cancel(op);
        if (tooLarge || headersTooLarge)
        {
            if (callbacks.failed)
                callbacks.failed(AuthFailure::ResponseLimit);
        }
        else if (redirect)
        {
            if (callbacks.failed)
                callbacks.failed(AuthFailure::RedirectRejected);
        }
        else if (error == QNetworkReply::OperationCanceledError)
        {
            if (callbacks.failed)
                callbacks.failed(AuthFailure::Cancelled);
        }
        else if (status == 0 ||
                 (error != QNetworkReply::NoError && !(status >= 400 && status <= 599)))
        {
            if (callbacks.failed)
                callbacks.failed(AuthFailure::Network);
        }
        else if (callbacks.complete)
        {
            callbacks.complete(std::move(response));
        }
    });
    operation->timer->start(20 * 1000);
    return std::make_unique<Handle>(state, operation);
}

}  // namespace chatterino::rumble
