// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/accounts/Account.hpp"

#include <QByteArray>
#include <QString>

namespace chatterino {

class RumbleAccount final : public Account
{
public:
    RumbleAccount(QString userID, QString username);
    ~RumbleAccount() override;

    RumbleAccount(const RumbleAccount &) = delete;
    RumbleAccount &operator=(const RumbleAccount &) = delete;

    QString toString() const override;
    const QString &userID() const;
    const QString &username() const;
    bool ready() const;

    QByteArray credentialCopy() const;
    void setCredential(QByteArray credential);
    void clearCredential() noexcept;
    void setUsername(QString username);

private:
    QString userID_;
    QString username_;
    QByteArray credential_;
};

}  // namespace chatterino
