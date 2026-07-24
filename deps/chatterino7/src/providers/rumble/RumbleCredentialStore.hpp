// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

class QObject;

namespace chatterino::rumble {

enum class CredentialStoreError : std::uint8_t {
    None,
    NotFound,
    Unavailable,
    AccessDenied,
    Failed,
};

struct CredentialReadResult {
    CredentialStoreError error = CredentialStoreError::Failed;
    QByteArray secret;
};

/// Asynchronous password-equivalent storage. Implementations must never use a
/// plaintext fallback.
class CredentialStore
{
public:
    using ReadCallback = std::function<void(CredentialReadResult)>;
    using WriteCallback = std::function<void(CredentialStoreError)>;

    virtual ~CredentialStore() = default;

    [[nodiscard]] virtual bool available() const = 0;
    virtual void read(QString key, ReadCallback callback) = 0;
    virtual void write(QString key, QByteArray secret,
                       WriteCallback callback) = 0;
    virtual void erase(QString key, WriteCallback callback) = 0;
};

std::unique_ptr<CredentialStore> makeCredentialStore(QObject *owner);
QString credentialStoreErrorText(CredentialStoreError error);

}  // namespace chatterino::rumble
