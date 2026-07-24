// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleApi.hpp"

#include <QNetworkRequest>
#include <QObject>

#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace chatterino::rumble {

// Concrete production transport for the anonymous read-only Rumble boundary.
//
// The transport owns a dedicated QNetworkAccessManager unless one is injected.
// Construct, start, cancel, and destroy it on the transport's thread. An
// injected manager and every reply it returns must have that same affinity.
// User callbacks are queued, so none can run before start() returns.
//
// Manager teardown destroys its owned replies; the transport silently retires
// their handles and suppresses callbacks without calling abort(). Destroying
// the owner or transport while the manager lives aborts active replies and
// suppresses queued callbacks.
class RumbleQtTransport final : public QObject, public Transport
{
public:
    struct State;

    explicit RumbleQtTransport(QObject *owner);
    RumbleQtTransport(QNetworkAccessManager &manager, QObject *owner);
    ~RumbleQtTransport() override;

    RumbleQtTransport(const RumbleQtTransport &) = delete;
    RumbleQtTransport &operator=(const RumbleQtTransport &) = delete;

    std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) override;

    // This is the same validation and attribute construction path used by
    // start(). It is public so deterministic tests can inspect Qt request
    // policy without making a network request.
    [[nodiscard]] static std::optional<QNetworkRequest> prepareRequest(
        const TransportRequest &request);

    [[nodiscard]] static bool isAllowedUrl(const QUrl &url,
                                           ExpectedMediaType mediaType);

private:
    std::shared_ptr<State> state_;
};

}  // namespace chatterino::rumble
