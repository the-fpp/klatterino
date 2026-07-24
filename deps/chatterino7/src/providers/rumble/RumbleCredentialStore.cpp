// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleCredentialStore.hpp"

#include "providers/rumble/RumbleCredentialStorePrivate.hpp"

#include <QCoreApplication>
#include <QPointer>
#include <QtGlobal>

#include <utility>

namespace chatterino::rumble {
namespace {

constexpr auto SERVICE = "com.chatterino.chatterino.rumble";

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

bool isUnavailableLinuxBackendError(QStringView errorText)
{
    return errorText.contains(QStringLiteral("org.freedesktop.secrets"),
                              Qt::CaseInsensitive) ||
           errorText.contains(QStringLiteral("secret service"),
                              Qt::CaseInsensitive) ||
           errorText.contains(
               QStringLiteral("not provided by any .service files"),
               Qt::CaseInsensitive) ||
           ((errorText.contains(QStringLiteral("D-Bus"),
                                Qt::CaseInsensitive) ||
             errorText.contains(QStringLiteral("dbus"), Qt::CaseInsensitive)) &&
            (errorText.contains(QStringLiteral("autolaunch"),
                                Qt::CaseInsensitive) ||
             errorText.contains(QStringLiteral("not connected"),
                                Qt::CaseInsensitive) ||
             errorText.contains(QStringLiteral("session bus"),
                                Qt::CaseInsensitive)));
}

class QtCredentialStore final : public CredentialStore
{
public:
    explicit QtCredentialStore(QObject *owner)
        : owner_(owner ? owner : QCoreApplication::instance())
    {
    }

    bool available() const override
    {
        return QKeychain::isAvailable();
    }

    void read(QString key, ReadCallback callback) override
    {
        if (!this->owner_ || !this->available())
        {
            callback({.error = CredentialStoreError::Unavailable});
            return;
        }
        auto *job = new QKeychain::ReadPasswordJob(QString::fromLatin1(SERVICE),
                                                   this->owner_);
        job->setKey(key);
        job->setInsecureFallback(false);
        QObject::connect(
            job, &QKeychain::Job::finished, job,
            [job, callback = std::move(callback)](QKeychain::Job *) mutable {
                CredentialReadResult result{
                    .error = detail::mapCredentialStoreError(
                        job->error(), job->errorString()),
                };
                if (result.error == CredentialStoreError::None)
                {
                    result.secret = job->binaryData();
                }
                callback(std::move(result));
            });
        job->start();
    }

    void write(QString key, QByteArray secret, WriteCallback callback) override
    {
        if (!this->owner_ || !this->available())
        {
            wipe(secret);
            callback(CredentialStoreError::Unavailable);
            return;
        }
        auto *job = new QKeychain::WritePasswordJob(
            QString::fromLatin1(SERVICE), this->owner_);
        job->setKey(key);
        job->setInsecureFallback(false);
        job->setBinaryData(secret);
        wipe(secret);
        QObject::connect(
            job, &QKeychain::Job::finished, job,
            [job, callback = std::move(callback)](QKeychain::Job *) mutable {
                callback(detail::mapCredentialStoreError(
                    job->error(), job->errorString()));
            });
        job->start();
    }

    void erase(QString key, WriteCallback callback) override
    {
        if (!this->owner_ || !this->available())
        {
            callback(CredentialStoreError::Unavailable);
            return;
        }
        auto *job = new QKeychain::DeletePasswordJob(
            QString::fromLatin1(SERVICE), this->owner_);
        job->setKey(key);
        job->setInsecureFallback(false);
        QObject::connect(
            job, &QKeychain::Job::finished, job,
            [job, callback = std::move(callback)](QKeychain::Job *) mutable {
                auto error = detail::mapCredentialStoreError(
                    job->error(), job->errorString());
                if (error == CredentialStoreError::NotFound)
                {
                    error = CredentialStoreError::None;
                }
                callback(error);
            });
        job->start();
    }

private:
    QPointer<QObject> owner_;
};

}  // namespace

namespace detail {

CredentialStoreError mapCredentialStoreError(QKeychain::Error error,
                                              QStringView errorText)
{
    switch (error)
    {
        case QKeychain::NoError:
            return CredentialStoreError::None;
        case QKeychain::EntryNotFound:
            return CredentialStoreError::NotFound;
        case QKeychain::AccessDenied:
        case QKeychain::AccessDeniedByUser:
            return CredentialStoreError::AccessDenied;
        case QKeychain::NoBackendAvailable:
        case QKeychain::NotImplemented:
            return CredentialStoreError::Unavailable;
        case QKeychain::CouldNotDeleteEntry:
            return CredentialStoreError::Failed;
        case QKeychain::OtherError:
            return isUnavailableLinuxBackendError(errorText)
                       ? CredentialStoreError::Unavailable
                       : CredentialStoreError::Failed;
    }
    return CredentialStoreError::Failed;
}

}  // namespace detail

std::unique_ptr<CredentialStore> makeCredentialStore(QObject *owner)
{
    return std::make_unique<QtCredentialStore>(owner);
}

QString credentialStoreErrorText(CredentialStoreError error)
{
    switch (error)
    {
        case CredentialStoreError::None:
            return {};
        case CredentialStoreError::NotFound:
            return QStringLiteral("Sign in to this Rumble account again.");
        case CredentialStoreError::Unavailable:
#if defined(Q_OS_LINUX)
            return QStringLiteral(
                "Secure credential storage is unavailable. Unlock or start "
                "your system keyring, then sign in again.");
#else
            return QStringLiteral(
                "Secure credential storage is unavailable. Rumble sign-in "
                "was not saved.");
#endif
        case CredentialStoreError::AccessDenied:
            return QStringLiteral(
                "Chatterino can't access secure credential storage. Check "
                "its permissions and try again.");
        case CredentialStoreError::Failed:
            return QStringLiteral(
                "Chatterino couldn't save this Rumble sign-in. Try again.");
    }
    return QStringLiteral(
        "Chatterino couldn't save this Rumble sign-in. Try again.");
}

}  // namespace chatterino::rumble
