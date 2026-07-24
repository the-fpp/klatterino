// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleSession.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace chatterino::rumble {
namespace {

void wipe(QByteArray &value) noexcept
{
    volatile char *bytes = value.data();
    for (qsizetype i = 0; i < value.size(); ++i)
    {
        bytes[i] = 0;
    }
    value.clear();
    value.squeeze();
}

bool safeBearer(const QByteArray &value)
{
    return !value.isEmpty() && value.size() <= 4096 &&
           std::ranges::none_of(value, [](unsigned char ch) {
               return ch <= 0x20 || ch == 0x7f || ch == ';' || ch == ',';
           });
}

bool jsonMedia(const QByteArray &value)
{
    const auto essence = value.toLower().split(';').front().trimmed();
    return essence == QByteArrayLiteral("application/json") ||
           (essence.startsWith("application/") && essence.endsWith("+json"));
}

bool eventStreamMedia(const QByteArray &value)
{
    return value.toLower().split(';').front().trimmed() ==
           QByteArrayLiteral("text/event-stream");
}

constexpr std::size_t MAX_EMOTE_ELIGIBILITY_STREAMS = 256;

std::optional<QJsonObject> object(const AuthResponse &response)
{
    if (!jsonMedia(response.contentType) || response.body.size() > 64 * 1024)
    {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(response.body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    return document.object();
}

bool safeIdentityText(const QString &value, qsizetype maximum)
{
    return !value.isEmpty() && value.size() <= maximum &&
           std::ranges::none_of(value, [](QChar ch) {
               return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
           });
}

std::optional<SessionIdentity> probeIdentity(const AuthResponse &response)
{
    const auto root = object(response);
    if (!root)
        return std::nullopt;
    const auto userValue = root->value(QStringLiteral("user"));
    if (!userValue.isObject())
        return std::nullopt;
    const auto user = userValue.toObject();
    const auto idValue = user.value(QStringLiteral("id"));
    QString id;
    if (idValue.isString())
        id = idValue.toString();
    else if (idValue.isDouble())
    {
        const auto numericID = idValue.toInteger();
        if (numericID > 0)
            id = QString::number(numericID);
    }
    if (!safeIdentityText(id, 128))
        return std::nullopt;

    auto username = user.value(QStringLiteral("username")).toString();
    if (!username.isEmpty() && !safeIdentityText(username, 256))
        return std::nullopt;
    return SessionIdentity{
        .userID = std::move(id),
        .username = std::move(username),
    };
}

std::optional<EmoteEligibility> eligibilityFromInit(
    const AuthResponse &response, const QString &userID)
{
    if (!eventStreamMedia(response.contentType) || response.body.isEmpty() ||
        response.body.size() > 1024 * 1024)
    {
        return std::nullopt;
    }
    QByteArray data;
    const auto lines = response.body.split('\n');
    for (auto line : lines)
    {
        if (line.endsWith('\r'))
        {
            line.chop(1);
        }
        if (line.isEmpty())
        {
            break;
        }
        if (!line.startsWith("data:"))
        {
            continue;
        }
        line.remove(0, 5);
        if (line.startsWith(' '))
        {
            line.remove(0, 1);
        }
        if (!data.isEmpty())
        {
            data.append('\n');
        }
        data.append(line);
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const auto event = document.object();
    if (event.value(QStringLiteral("type")).toString() !=
        QStringLiteral("init"))
    {
        return std::nullopt;
    }
    const auto dataObject = event.value(QStringLiteral("data")).toObject();
    const auto usersValue = dataObject.value(QStringLiteral("users"));
    if (!usersValue.isArray())
    {
        return std::nullopt;
    }
    const auto users = usersValue.toArray();
    if (users.size() > 50000)
    {
        return std::nullopt;
    }
    for (const auto &value : users)
    {
        if (!value.isObject())
        {
            continue;
        }
        const auto user = value.toObject();
        const auto idValue = user.value(QStringLiteral("id"));
        QString id;
        if (idValue.isString())
        {
            id = idValue.toString();
        }
        else if (idValue.isDouble() && idValue.toInteger() > 0)
        {
            id = QString::number(idValue.toInteger());
        }
        if (id != userID)
        {
            continue;
        }
        const auto follower = user.value(QStringLiteral("is_follower"));
        if (!follower.isBool())
        {
            return std::nullopt;
        }
        bool privileged = false;
        const auto badges = user.value(QStringLiteral("badges"));
        if (!badges.isUndefined() && !badges.isArray())
        {
            return std::nullopt;
        }
        for (const auto &badge : badges.toArray())
        {
            if (!badge.isString())
            {
                return std::nullopt;
            }
            const auto id = badge.toString();
            privileged |= id == QStringLiteral("admin") ||
                          id == QStringLiteral("recurring_subscription") ||
                          id == QStringLiteral("locals_supporter");
        }
        return EmoteEligibility{
            .following = follower.toBool(),
            .subscriberOrAdmin = privileged,
        };
    }
    return std::nullopt;
}

std::optional<QString> sentId(const AuthResponse &response)
{
    const auto root = object(response);
    if (!root)
        return std::nullopt;
    const auto data = root->value(QStringLiteral("data"));
    if (!data.isObject())
        return std::nullopt;
    const auto id = data.toObject().value(QStringLiteral("id"));
    if (id.isString() && !id.toString().isEmpty())
        return id.toString();
    if (id.isDouble() && id.toDouble() != 0)
        return QString::number(id.toInteger());
    return std::nullopt;
}

std::optional<QDateTime> retryDeadline(const QByteArray &raw,
                                       const QDateTime &now)
{
    bool ok = false;
    const auto seconds = raw.trimmed().toLongLong(&ok);
    if (ok && seconds >= 0 && seconds <= 24 * 60 * 60)
        return now.addSecs(seconds);
    const auto date =
        QDateTime::fromString(QString::fromLatin1(raw), Qt::RFC2822Date);
    if (date.isValid() && date > now && now.secsTo(date) <= 24 * 60 * 60)
        return date;
    return std::nullopt;
}

QString definiteText()
{
    return QStringLiteral(
        "Rumble rejected the message. Your draft was preserved.");
}
QString ambiguousText()
{
    return QStringLiteral("Rumble may have sent this message. It was not "
                          "retried; check chat before sending again.");
}

}  // namespace

struct SessionController::State : std::enable_shared_from_this<State> {
    AuthTransport &transport;
    Clock clock;
    Changed changed;
    QByteArray bearer;
    SessionState session = SessionState::Empty;
    std::uint64_t generation = 0;
    std::uint64_t eligibilityGeneration = 0;
    std::unique_ptr<AuthHandle> probe;
    std::unique_ptr<AuthHandle> send;
    std::map<QString, std::unique_ptr<AuthHandle>> eligibilityRequests;
    std::map<QString, EmoteEligibility> eligibilities;
    bool sending = false;
    QString deniedStream;
    std::optional<SessionIdentity> identity;
    std::optional<QDateTime> blocked;
    bool shuttingDown = false;

    State(AuthTransport &transport, Clock clock)
        : transport(transport)
        , clock(clock ? std::move(clock) : [] {
            return QDateTime::currentDateTimeUtc();
        })
    {
    }

    ~State()
    {
        clear();
    }

    void notify()
    {
        if (changed)
            changed();
    }

    void cancelOperations() noexcept
    {
        if (probe)
            probe->cancel();
        if (send)
            send->cancel();
        for (auto &[stream, request] : eligibilityRequests)
        {
            (void)stream;
            if (request)
                request->cancel();
        }
        probe.reset();
        send.reset();
        eligibilityRequests.clear();
        eligibilities.clear();
        sending = false;
    }

    void clear() noexcept
    {
        ++generation;
        ++eligibilityGeneration;
        cancelOperations();
        wipe(bearer);
        session = SessionState::Empty;
        identity.reset();
        deniedStream.clear();
        blocked.reset();
        notify();
    }
};

SessionController::SessionController(AuthTransport &transport, Clock clock)
    : state_(std::make_shared<State>(transport, std::move(clock)))
{
}

SessionController::~SessionController()
{
    shutdown();
}

bool SessionController::importSession(QByteArray bearer)
{
    if (!state_ || state_->shuttingDown || !safeBearer(bearer))
    {
        wipe(bearer);
        return false;
    }
    state_->clear();
    state_->bearer = std::move(bearer);
    state_->session = SessionState::Unvalidated;
    state_->notify();
    return true;
}

void SessionController::clear() noexcept
{
    if (state_)
        state_->clear();
}

void SessionController::shutdown() noexcept
{
    if (!state_ || state_->shuttingDown)
        return;
    state_->shuttingDown = true;
    state_->clear();
}

void SessionController::validate(ProbeCallback callback)
{
    const auto state = state_;
    if (!state || state->shuttingDown || state->bearer.isEmpty() ||
        state->session == SessionState::Validating || state->sending ||
        (state->blocked && *state->blocked > state->clock()))
    {
        if (callback)
            callback(false,
                     QStringLiteral("Rumble sign-in is not ready yet. Try "
                                    "again."));
        return;
    }
    if (state->probe)
        state->probe->cancel();
    state->probe.reset();
    const auto generation = ++state->generation;
    state->session = SessionState::Validating;
    state->notify();
    const auto completedInline = std::make_shared<bool>(false);
    const auto startReturned = std::make_shared<bool>(false);
    const std::weak_ptr weak = state;
    auto finish = [weak, generation, completedInline, startReturned,
                   callback = std::move(callback)](
                      std::optional<AuthResponse> response,
                      std::optional<AuthFailure> failure) mutable {
        const auto self = weak.lock();
        if (!self || self->shuttingDown || self->generation != generation)
            return;
        if (*completedInline)
            return;
        *completedInline = true;
        if (*startReturned)
            self->probe.reset();
        const auto identity = response && response->status == 200
                                  ? probeIdentity(*response)
                                  : std::optional<SessionIdentity>{};
        bool accepted = identity.has_value();
        if (response && (response->status == 401 || response->status == 403))
        {
            self->clear();
            if (callback)
                callback(
                    false,
                    QStringLiteral("Rumble sign-in was rejected. Sign in "
                                   "again."));
            return;
        }
        if (response && response->status == 429)
        {
            self->blocked =
                response->retryAfter
                    ? retryDeadline(*response->retryAfter, self->clock())
                    : std::nullopt;
            if (!self->blocked)
                self->blocked = self->clock().addSecs(60);
        }
        else if (accepted)
        {
            self->blocked.reset();
            self->identity = identity;
        }
        self->session =
            accepted ? SessionState::Valid : SessionState::Unvalidated;
        self->notify();
        if (callback)
        {
            callback(accepted,
                     accepted ? QStringLiteral("Rumble sign-in confirmed.")
                              : QStringLiteral("Rumble sign-in couldn't be "
                                               "confirmed. Try again."));
        }
        (void)failure;
    };
    auto handle = state->transport.start(
        AuthOperation::Probe, {}, {}, state->bearer, {},
        {.complete =
             [finish](AuthResponse response) mutable {
                 finish(std::move(response), std::nullopt);
             },
         .failed =
             [finish](AuthFailure failure) mutable {
                 finish(std::nullopt, failure);
             }});
    *startReturned = true;
    if (!*completedInline)
        state->probe = std::move(handle);
    if (!handle && !state->probe && !*completedInline)
    {
        finish(std::nullopt, AuthFailure::InvalidRequest);
    }
}

void SessionController::send(QString streamId, QString text,
                             SendCallback callback)
{
    const auto state = state_;
    const auto fail = [&](QString message) {
        if (callback)
            callback({SendOutcome::DefiniteFailure, std::move(message), {}});
    };
    if (!state || state->shuttingDown || state->session != SessionState::Valid)
        return fail(QStringLiteral("Sign in to Rumble before sending."));
    if (streamId.isEmpty() || streamId != streamId.trimmed() ||
        !std::ranges::all_of(streamId, [](QChar ch) {
            return ch.isDigit();
        }))
        return fail(QStringLiteral(
            "This Rumble channel is unavailable for sending."));
    if (text.isEmpty() || text.size() > ABSOLUTE_TEXT_LIMIT ||
        std::ranges::any_of(text, [](QChar ch) {
            return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
        }))
        return fail(
            QStringLiteral("This message is empty or too long for Rumble."));
    const auto now = state->clock();
    if ((state->blocked && *state->blocked > now) ||
        state->deniedStream == streamId)
        return fail(
            QStringLiteral("This Rumble channel can't receive messages yet."));
    if (state->sending)
        return fail(QStringLiteral("Your Rumble message is still sending."));

    const auto generation = ++state->generation;
    state->sending = true;
    state->notify();
    QByteArray entropy(32, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < entropy.size();
         offset += static_cast<qsizetype>(sizeof(quint32)))
    {
        const auto word = QRandomGenerator::system()->generate();
        std::memcpy(entropy.data() + offset, &word, sizeof(word));
    }
    const auto requestId = entropy.toBase64(QByteArray::Base64UrlEncoding |
                                            QByteArray::OmitTrailingEquals);
    wipe(entropy);
    const auto completedInline = std::make_shared<bool>(false);
    const auto startReturned = std::make_shared<bool>(false);
    const std::weak_ptr weak = state;
    auto finish = [weak, generation, streamId, completedInline, startReturned,
                   callback = std::move(callback)](
                      std::optional<AuthResponse> response,
                      std::optional<AuthFailure> failure) mutable {
        const auto self = weak.lock();
        if (!self || self->shuttingDown || self->generation != generation)
            return;
        if (*completedInline)
            return;
        *completedInline = true;
        if (*startReturned)
            self->send.reset();
        self->sending = false;
        SendResult result;
        if (failure)
        {
            result.outcome = *failure == AuthFailure::Cancelled
                                 ? SendOutcome::Cancelled
                             : *failure == AuthFailure::InvalidRequest
                                 ? SendOutcome::DefiniteFailure
                                 : SendOutcome::Ambiguous;
            result.userMessage =
                *failure == AuthFailure::Cancelled
                    ? QStringLiteral("Rumble send was cancelled.")
                : *failure == AuthFailure::InvalidRequest
                    ? QStringLiteral("Rumble couldn't send the message. Try "
                                     "again.")
                    : ambiguousText();
        }
        else if (response->status == 200)
        {
            result.messageId = sentId(*response);
            result.outcome = result.messageId ? SendOutcome::Confirmed
                                              : SendOutcome::Ambiguous;
            result.userMessage = result.messageId ? QString{} : ambiguousText();
        }
        else if (response->status == 408 || response->status >= 500)
        {
            result.outcome = SendOutcome::Ambiguous;
            result.userMessage = ambiguousText();
        }
        else
        {
            result.outcome = SendOutcome::DefiniteFailure;
            result.userMessage = definiteText();
            if (response->status == 401)
            {
                self->session = SessionState::Unvalidated;
            }
            else if (response->status == 403)
            {
                self->deniedStream = streamId;
            }
            else if (response->status == 429 && response->retryAfter)
            {
                self->blocked =
                    retryDeadline(*response->retryAfter, self->clock());
            }
            if (response->status == 429 && !self->blocked)
            {
                // Invalid/missing Retry-After is not trusted, but a bounded
                // conservative delay still prevents local request storms.
                self->blocked = self->clock().addSecs(60);
            }
        }
        // Publish one fully classified snapshot. In particular, a 401 may not
        // expose a reentrant writable window between retiring the operation
        // and invalidating validation.
        self->notify();
        if (callback)
            callback(std::move(result));
    };
    auto handle = state->transport.start(
        AuthOperation::Send, std::move(streamId), std::move(text),
        state->bearer, requestId,
        {.complete =
             [finish](AuthResponse response) mutable {
                 finish(std::move(response), std::nullopt);
             },
         .failed =
             [finish](AuthFailure failure) mutable {
                 finish(std::nullopt, failure);
             }});
    *startReturned = true;
    if (!*completedInline)
        state->send = std::move(handle);
    if (!handle && !state->send && !*completedInline)
        finish(std::nullopt, AuthFailure::InvalidRequest);
}

SessionState SessionController::state() const noexcept
{
    return state_ ? state_->session : SessionState::Empty;
}

bool SessionController::isWritable(const QString &streamId) const
{
    if (!state_ || state_->shuttingDown ||
        state_->session != SessionState::Valid || state_->sending ||
        state_->deniedStream == streamId)
        return false;
    return !state_->blocked || *state_->blocked <= state_->clock();
}

QString SessionController::accountId() const
{
    if (!state_ || state_->session == SessionState::Empty)
        return {};
    return state_->identity ? state_->identity->userID
                            : QStringLiteral("rumble-imported-session");
}

std::optional<SessionIdentity> SessionController::identity() const
{
    if (!state_ || state_->session == SessionState::Empty)
        return std::nullopt;
    return state_->identity;
}

void SessionController::ensureEmoteEligibility(QString streamId)
{
    const auto state = state_;
    if (!state || state->shuttingDown ||
        state->session != SessionState::Valid || !state->identity ||
        streamId.isEmpty() || streamId.size() > 128 ||
        streamId.front() == u'0' ||
        !std::ranges::all_of(streamId,
                             [](QChar ch) {
                                 return ch.isDigit();
                             }) ||
        state->eligibilities.contains(streamId) ||
        state->eligibilityRequests.contains(streamId) ||
        state->eligibilities.size() + state->eligibilityRequests.size() >=
            MAX_EMOTE_ELIGIBILITY_STREAMS)
    {
        return;
    }

    const auto generation = state->eligibilityGeneration;
    const auto userID = state->identity->userID;
    const auto completedInline = std::make_shared<bool>(false);
    const auto startReturned = std::make_shared<bool>(false);
    const std::weak_ptr weak = state;
    auto finish = [weak, generation, streamId, userID, completedInline,
                   startReturned](std::optional<AuthResponse> response,
                                  std::optional<AuthFailure>) mutable {
        const auto self = weak.lock();
        if (!self || self->shuttingDown ||
            self->eligibilityGeneration != generation || *completedInline)
        {
            return;
        }
        *completedInline = true;
        if (*startReturned)
        {
            self->eligibilityRequests.erase(streamId);
        }
        if (response && (response->status == 401 || response->status == 403))
        {
            self->clear();
            return;
        }
        if (!response || response->status != 200)
        {
            return;
        }
        auto eligibility = eligibilityFromInit(*response, userID);
        if (!eligibility)
        {
            return;
        }
        self->eligibilities.insert_or_assign(streamId, *eligibility);
        self->notify();
    };
    auto handle = state->transport.start(
        AuthOperation::Eligibility, streamId, {}, state->bearer, {},
        {.complete =
             [finish](AuthResponse response) mutable {
                 finish(std::move(response), std::nullopt);
             },
         .failed =
             [finish](AuthFailure failure) mutable {
                 finish(std::nullopt, failure);
             }});
    *startReturned = true;
    if (!*completedInline && handle)
    {
        state->eligibilityRequests.emplace(std::move(streamId),
                                           std::move(handle));
    }
    else if (!handle && !*completedInline)
    {
        finish(std::nullopt, AuthFailure::InvalidRequest);
    }
}

std::optional<EmoteEligibility> SessionController::emoteEligibility(
    const QString &streamId) const
{
    if (!state_ || state_->shuttingDown ||
        state_->session != SessionState::Valid)
    {
        return std::nullopt;
    }
    const auto found = state_->eligibilities.find(streamId);
    return found == state_->eligibilities.end()
               ? std::optional<EmoteEligibility>{}
               : std::optional<EmoteEligibility>{found->second};
}

std::optional<QDateTime> SessionController::blockedUntil() const
{
    return state_ ? state_->blocked : std::nullopt;
}

SessionDiagnosticSnapshot SessionController::diagnosticSnapshot(
    const QString &streamId) const
{
    if (!state_)
        return {};
    return {
        .state = state_->session,
        .blockedUntilUtc = state_->blocked,
        .destinationDenied =
            !streamId.isEmpty() && state_->deniedStream == streamId,
        .sendInProgress = state_->sending,
    };
}

std::uint64_t SessionController::generation() const noexcept
{
    return state_ ? state_->generation : 0;
}

void SessionController::setChanged(Changed changed)
{
    if (state_)
        state_->changed = std::move(changed);
}

}  // namespace chatterino::rumble
