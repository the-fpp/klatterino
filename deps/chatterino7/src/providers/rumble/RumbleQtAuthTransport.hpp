// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleSession.hpp"

#include <QObject>

#include <memory>

class QNetworkAccessManager;

namespace chatterino::rumble {

class RumbleQtAuthTransport final : public QObject, public AuthTransport
{
public:
    struct State;
    explicit RumbleQtAuthTransport(QObject *owner);
    RumbleQtAuthTransport(QNetworkAccessManager &manager, QObject *owner);
    ~RumbleQtAuthTransport() override;

    std::unique_ptr<AuthHandle> start(AuthOperation operation,
                                      QString streamId, QString text,
                                      QByteArray bearer, QByteArray requestId,
                                      AuthCallbacks callbacks) override;

private:
    std::shared_ptr<State> state_;
};

}  // namespace chatterino::rumble
