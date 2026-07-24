// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleAccount.hpp"

#include <utility>

namespace chatterino {
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

}  // namespace

RumbleAccount::RumbleAccount(QString userID, QString username)
    : Account(ProviderId::Rumble)
    , userID_(std::move(userID))
    , username_(std::move(username))
{
}

RumbleAccount::~RumbleAccount()
{
    this->clearCredential();
}

QString RumbleAccount::toString() const
{
    return this->username_;
}

const QString &RumbleAccount::userID() const
{
    return this->userID_;
}

const QString &RumbleAccount::username() const
{
    return this->username_;
}

bool RumbleAccount::ready() const
{
    return !this->credential_.isEmpty();
}

QByteArray RumbleAccount::credentialCopy() const
{
    return this->credential_;
}

void RumbleAccount::setCredential(QByteArray credential)
{
    this->clearCredential();
    this->credential_ = std::move(credential);
}

void RumbleAccount::clearCredential() noexcept
{
    wipe(this->credential_);
}

void RumbleAccount::setUsername(QString username)
{
    this->username_ = std::move(username);
}

}  // namespace chatterino
