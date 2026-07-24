// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleEvent.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::rumble {

inline constexpr int MAX_RESPONSE_HEADERS = 96;
inline constexpr qsizetype MAX_RESPONSE_HEADER_BYTES = 64 * 1024;

enum class LocatorKind {
    Channel,
    Video,
    VideoPage,
    Stream,
};

struct Locator {
    LocatorKind kind = LocatorKind::Channel;
    // Channel slug, embed/video ID, or decimal stream ID. Stored losslessly.
    QString value;
    // Present only for a supported public video page.
    QString pagePath;
};

enum class Outcome {
    ResolvedLive,
    ValidOffline,
    NotFound,
    UnsupportedInput,
    AccessInterstitial,
    TransportError,
    HttpError,
    RateLimited,
    Timeout,
    Cancelled,
    MalformedSchema,
    LimitExceeded,
    RedirectRejected,
    InvalidMediaType,
};

struct RetryMetadata {
    std::optional<std::chrono::seconds> after;
    bool retryable = false;
};

struct Error {
    Outcome outcome = Outcome::TransportError;
    int httpStatus = 0;
    RetryMetadata retry;
    // Stable, sanitized code only. It never contains a URL, locator, provider
    // identifier, response fragment, header value, or Qt error string.
    QString code;
};

struct Metadata {
    // Empty unless the documented response supplies a channel identity.
    QString channelIdentity;
    // Empty unless the documented embed response supplies a channel title.
    // A channel/profile page's document title belongs to its current video.
    QString channelTitle;
    QString embedId;
    QString videoTitle;
    // Decimal provider ID retained as a string to avoid numeric truncation.
    QString streamId;
};

struct ResolveResult {
    Outcome outcome = Outcome::TransportError;
    Locator locator;
    std::optional<Metadata> metadata;
    std::optional<Error> error;
};

struct BootstrapResult {
    Outcome outcome = Outcome::TransportError;
    // Provider consumers receive only #10's typed events. Raw SSE fields and
    // JSON records never cross this boundary.
    std::vector<Event> events;
    // Sanitized parser diagnostics contain stable codes and schema paths only.
    std::vector<Diagnostic> diagnostics;
    std::optional<Error> error;
};

struct EmoteCatalogResult {
    Outcome outcome = Outcome::TransportError;
    std::optional<EmoteCatalog> catalog;
    std::vector<Diagnostic> diagnostics;
    std::optional<Error> error;
};

struct StreamBatch {
    std::vector<Event> events;
    std::vector<Diagnostic> diagnostics;
};

struct StreamTerminal {
    Outcome outcome = Outcome::TransportError;
    std::optional<Error> error;
};

struct StreamCallbacks {
    // Raw SSE fields and JSON records never cross this boundary. Both each
    // batch and the number of queued handoffs are bounded.
    std::function<void(StreamBatch)> onEvents;
    // Called exactly once after the final batch. Clean HTTP 200 EOF is a
    // retryable stream end; HTTP 204 is a typed ValidOffline terminal.
    std::function<void(StreamTerminal)> onTerminal;
};

enum class ExpectedMediaType {
    Html,
    Json,
    EventStream,
};

enum class DeadlineScope {
    WholeResponse,
    UntilFinalHead,
};

enum class BodyLimitScope {
    // maxBodyBytes bounds all decoded bytes received for a finite response.
    Cumulative,
    // maxBodyBytes/maxPendingBodyChunks bound only bytes queued between the
    // transport and consumer. A full window pauses and later resumes reading.
    PendingDelivery,
};

struct Header {
    QByteArray name;
    QByteArray value;
};

struct TransportRequest {
    QUrl url;
    std::vector<Header> headers;
    ExpectedMediaType expectedMediaType = ExpectedMediaType::Json;
    qsizetype maxBodyBytes = 1024 * 1024;
    int maxHeaders = MAX_RESPONSE_HEADERS;
    qsizetype maxHeaderBytes = MAX_RESPONSE_HEADER_BYTES;
    int timeoutMs = 20 * 1000;
    int maxRedirects = 3;
    DeadlineScope deadlineScope = DeadlineScope::WholeResponse;
    BodyLimitScope bodyLimitScope = BodyLimitScope::Cumulative;
    int maxPendingBodyChunks = 64;
};

struct ResponseHead {
    int status = 0;
    std::vector<Header> headers;
};

enum class TransportFailure {
    Network,
    Timeout,
    Cancelled,
    RedirectRejected,
    InvalidMediaType,
    BodyLimit,
    HeaderLimit,
    OwnerDestroyed,
};

struct TransportCallbacks {
    std::function<void(const ResponseHead &)> onHead;
    std::function<void(const QByteArray &)> onBodyChunk;
    std::function<void()> onComplete;
    std::function<void(TransportFailure)> onFailure;
};

class TransportHandle
{
public:
    virtual ~TransportHandle() = default;
    virtual void cancel() noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
};

class Transport
{
public:
    virtual ~Transport() = default;

    // Implementations must serialize start(), cancel(), and callbacks on the
    // API's owning thread and suppress callbacks after cancel(). A completion
    // may happen synchronously; RumbleApi still defers all consumer callbacks
    // until after its Cancellation has been returned.
    virtual std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) = 0;
};

namespace detail {
struct ApiOperation;
}

class Cancellation final
{
public:
    Cancellation() = default;
    Cancellation(const Cancellation &) = delete;
    Cancellation &operator=(const Cancellation &) = delete;
    Cancellation(Cancellation &&other) noexcept;
    Cancellation &operator=(Cancellation &&other) noexcept;
    ~Cancellation();

    // Cancellation is idempotent. It aborts the current transport request and
    // suppresses any queued consumer callback. Destroying this handle cancels.
    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    explicit Cancellation(std::shared_ptr<detail::ApiOperation> operation);

    std::shared_ptr<detail::ApiOperation> operation_;

    friend class RumbleApi;
};

class RumbleApi final
{
public:
    using Defer = std::function<void(std::function<void()>)>;
    using Clock = std::function<QDateTime()>;
    using ResolveCallback = std::function<void(ResolveResult)>;
    using BootstrapCallback = std::function<void(BootstrapResult)>;
    using EmoteCatalogCallback = std::function<void(EmoteCatalogResult)>;

    // transport must outlive this API and all returned Cancellation handles.
    // defer must enqueue work rather than invoke it inline.
    RumbleApi(Transport &transport, Defer defer, Clock clock = Clock{});
    ~RumbleApi();

    RumbleApi(const RumbleApi &) = delete;
    RumbleApi &operator=(const RumbleApi &) = delete;

    [[nodiscard]] static std::optional<Locator> normalizeLocator(
        const QString &input);
    [[nodiscard]] static std::optional<std::chrono::seconds> parseRetryAfter(
        const QByteArray &value, const QDateTime &now);

    [[nodiscard]] Cancellation resolve(QString input, ResolveCallback callback);
    [[nodiscard]] Cancellation bootstrap(QString streamId,
                                         BootstrapCallback callback);
    [[nodiscard]] Cancellation emoteCatalog(QString streamId,
                                            EmoteCatalogCallback callback);
    [[nodiscard]] Cancellation stream(QString streamId,
                                      StreamCallbacks callbacks);

private:
    Transport &transport_;
    Defer defer_;
    Clock clock_;
    std::shared_ptr<bool> alive_;
    std::vector<std::weak_ptr<detail::ApiOperation>> operations_;
};

}  // namespace chatterino::rumble
