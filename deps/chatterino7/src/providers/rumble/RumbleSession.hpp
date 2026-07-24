// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace chatterino::rumble {

enum class AuthOperation : std::uint8_t { Probe, Eligibility, Send };
enum class AuthFailure : std::uint8_t {
    Network,
    Timeout,
    Cancelled,
    InvalidRequest,
    RedirectRejected,
    ResponseLimit,
};
struct AuthResponse {
    int status = 0;
    QByteArray contentType;
    QByteArray body;
    std::optional<QByteArray> retryAfter;
};
class AuthHandle
{
public:
    virtual ~AuthHandle() = default;
    virtual void cancel() noexcept = 0;
};
struct AuthCallbacks {
    std::function<void(AuthResponse)> complete;
    std::function<void(AuthFailure)> failed;
};

/// Authenticated transport boundary. The bearer is supplied only to this
/// boundary and must never be logged, persisted, returned, or included in an
/// error. Implementations accept only the three frozen schema-1 operations.
class AuthTransport
{
public:
    virtual ~AuthTransport() = default;
    virtual std::unique_ptr<AuthHandle> start(AuthOperation operation,
                                              QString streamId, QString text,
                                              QByteArray bearer,
                                              QByteArray requestId,
                                              AuthCallbacks callbacks) = 0;
};

enum class SessionState : std::uint8_t {
    Empty,
    Unvalidated,
    Validating,
    Valid,
};
enum class SendOutcome : std::uint8_t {
    Confirmed,
    DefiniteFailure,
    Ambiguous,
    Cancelled,
};
struct SendResult {
    SendOutcome outcome = SendOutcome::DefiniteFailure;
    QString userMessage;
    std::optional<QString> messageId;
};
struct SessionDiagnosticSnapshot {
    SessionState state = SessionState::Empty;
    std::optional<QDateTime> blockedUntilUtc;
    bool destinationDenied = false;
    bool sendInProgress = false;
};
struct SessionIdentity {
    QString userID;
    QString username;

    friend bool operator==(const SessionIdentity &,
                           const SessionIdentity &) = default;
};
struct EmoteEligibility {
    bool following = false;
    bool subscriberOrAdmin = false;

    friend bool operator==(const EmoteEligibility &,
                           const EmoteEligibility &) = default;
};

/// Memory-only imported-session lifecycle. It owns all bearer material,
/// wipes retired buffers, invalidates stale callbacks by generation, and
/// serializes one-shot sends without retrying them.
class SessionController final
{
public:
    using Clock = std::function<QDateTime()>;
    using Changed = std::function<void()>;
    using ProbeCallback = std::function<void(bool, QString)>;
    using SendCallback = std::function<void(SendResult)>;

    explicit SessionController(AuthTransport &transport, Clock clock = {});
    ~SessionController();
    SessionController(const SessionController &) = delete;
    SessionController &operator=(const SessionController &) = delete;

    bool importSession(QByteArray bearer);
    void clear() noexcept;
    void shutdown() noexcept;
    void validate(ProbeCallback callback);
    void send(QString streamId, QString text, SendCallback callback);

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool isWritable(const QString &streamId) const;
    [[nodiscard]] QString accountId() const;
    [[nodiscard]] std::optional<SessionIdentity> identity() const;
    void ensureEmoteEligibility(QString streamId);
    [[nodiscard]] std::optional<EmoteEligibility> emoteEligibility(
        const QString &streamId) const;
    [[nodiscard]] std::optional<QDateTime> blockedUntil() const;
    /// Owner-thread-only, identity-free copy for local diagnostics. The stream
    /// ID is used only for an equality check and is never retained or returned.
    [[nodiscard]] SessionDiagnosticSnapshot diagnosticSnapshot(
        const QString &streamId) const;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    void setChanged(Changed changed);

    static constexpr qsizetype ABSOLUTE_TEXT_LIMIT = 4096;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace chatterino::rumble
