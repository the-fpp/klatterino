// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleAccountManager.hpp"

#include "Application.hpp"
#include "providers/rumble/RumbleAccount.hpp"
#include "singletons/Settings.hpp"
#include "util/SharedPtrElementLess.hpp"

#include <pajlada/settings/settingmanager.hpp>
#include <QCoreApplication>
#include <QCryptographicHash>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

bool safeText(const QString &value, qsizetype maximum)
{
    return !value.isEmpty() && value.size() <= maximum &&
           std::ranges::none_of(value, [](QChar ch) {
               return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
           });
}

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

QString accountStorageID(const QString &userID)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(userID.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

std::string accountSettingsPath(const QString &userID)
{
    return "/rumbleAccounts/uid" + accountStorageID(userID).toStdString();
}

}  // namespace

RumbleAccountManager::RumbleAccountManager(
    QObject *owner, std::unique_ptr<rumble::CredentialStore> credentialStore)
    : accounts(SharedPtrElementLess<RumbleAccount>{})
    , credentialStore_(credentialStore
                           ? std::move(credentialStore)
                           : rumble::makeCredentialStore(
                                 owner ? owner : QCoreApplication::instance()))
    , lifetime_(std::make_shared<int>(0))
{
    std::ignore = this->accounts.itemRemoved.connect([this](const auto &event) {
        this->removeAccount(event.item.get());
    });
}

RumbleAccountManager::~RumbleAccountManager()
{
    this->lifetime_.reset();
}

bool RumbleAccountManager::validIdentity(const QString &userID,
                                         const QString &username)
{
    return safeText(userID, 128) && safeText(username, 256);
}

QString RumbleAccountManager::credentialKey(const QString &userID)
{
    return QStringLiteral("account/") + accountStorageID(userID);
}

bool RumbleAccountManager::secureStorageAvailable() const
{
    return this->credentialStore_ && this->credentialStore_->available();
}

std::shared_ptr<RumbleAccount> RumbleAccountManager::findByID(
    const QString &userID) const
{
    for (const auto &account : this->accounts.raw())
    {
        if (account->userID() == userID)
        {
            return account;
        }
    }
    return {};
}

std::vector<std::shared_ptr<RumbleAccount>> RumbleAccountManager::accountList()
    const
{
    return this->accounts.raw();
}

std::shared_ptr<RumbleAccount> RumbleAccountManager::current() const
{
    return this->current_;
}

std::optional<QByteArray> RumbleAccountManager::currentCredential() const
{
    if (!this->current_ || !this->current_->ready())
    {
        return std::nullopt;
    }
    return this->current_->credentialCopy();
}

void RumbleAccountManager::refreshCurrent()
{
    this->current_ = this->findByID(this->currentAccountID.getValue());
    this->currentUserChanged.invoke();
}

void RumbleAccountManager::selectAccount(const QString &userID)
{
    if (!userID.isEmpty() && !this->findByID(userID))
    {
        return;
    }
    this->currentAccountID = userID;
    if (this->current_ != this->findByID(userID))
    {
        this->refreshCurrent();
    }
    if (auto *settings = getSettings())
    {
        std::ignore = settings->requestSave();
    }
}

void RumbleAccountManager::load()
{
    if (this->loaded_)
    {
        return;
    }
    this->loaded_ = true;

    this->currentAccountID.connect([this](const QString &) {
        this->refreshCurrent();
    });

    const auto keys =
        pajlada::Settings::SettingManager::getObjectKeys("/rumbleAccounts");
    const std::weak_ptr<void> lifetime = this->lifetime_;
    for (const auto &key : keys)
    {
        if (key == "current" || !key.starts_with("uid"))
        {
            continue;
        }
        const auto basePath = "/rumbleAccounts/" + key;
        const auto userID =
            pajlada::Settings::Setting<QString>::get(basePath + "/userID");
        const auto username =
            pajlada::Settings::Setting<QString>::get(basePath + "/username");
        if (!this->validIdentity(userID, username) || this->findByID(userID))
        {
            continue;
        }

        auto account = std::make_shared<RumbleAccount>(userID, username);
        this->accounts.insert(account);
        this->credentialStore_->read(
            this->credentialKey(userID),
            [this, lifetime, account](rumble::CredentialReadResult result) {
                if (lifetime.expired())
                {
                    wipe(result.secret);
                    return;
                }
                if (result.error == rumble::CredentialStoreError::None)
                {
                    account->setCredential(std::move(result.secret));
                    this->userListUpdated.invoke();
                    if (this->currentAccountID.getValue() == account->userID())
                    {
                        this->refreshCurrent();
                    }
                    return;
                }
                wipe(result.secret);
                this->storageError.invoke(
                    rumble::credentialStoreErrorText(result.error));
            });
    }
    this->refreshCurrent();
    this->userListUpdated.invoke();
}

void RumbleAccountManager::addValidatedAccount(QString userID, QString username,
                                               QByteArray credential,
                                               AddCallback callback)
{
    if (!this->validIdentity(userID, username))
    {
        wipe(credential);
        callback({
            .userMessage =
                QStringLiteral("Rumble did not return a valid account name."),
        });
        return;
    }
    if (!this->secureStorageAvailable())
    {
        wipe(credential);
        callback({
            .userMessage = rumble::credentialStoreErrorText(
                rumble::CredentialStoreError::Unavailable),
        });
        return;
    }

    auto savedCredential = credential;
    const auto storageKey = this->credentialKey(userID);
    const std::weak_ptr<void> lifetime = this->lifetime_;
    this->credentialStore_->write(
        storageKey, std::move(credential),
        [this, lifetime, userID = std::move(userID),
         username = std::move(username),
         savedCredential = std::move(savedCredential),
         callback =
             std::move(callback)](rumble::CredentialStoreError error) mutable {
            if (lifetime.expired())
            {
                wipe(savedCredential);
                return;
            }
            if (error != rumble::CredentialStoreError::None)
            {
                wipe(savedCredential);
                callback({
                    .userMessage = rumble::credentialStoreErrorText(error),
                });
                return;
            }

            auto account = this->findByID(userID);
            if (!account)
            {
                account = std::make_shared<RumbleAccount>(userID, username);
                this->accounts.insert(account);
            }
            else
            {
                account->setUsername(username);
            }
            account->setCredential(std::move(savedCredential));

            const auto basePath = accountSettingsPath(account->userID());
            pajlada::Settings::Setting<QString>::set(basePath + "/userID",
                                                     account->userID());
            pajlada::Settings::Setting<QString>::set(basePath + "/username",
                                                     account->username());
            this->currentAccountID = account->userID();
            if (this->current_ != account)
            {
                this->refreshCurrent();
            }
            this->userListUpdated.invoke();
            if (auto *settings = getSettings())
            {
                std::ignore = settings->requestSave();
            }
            callback({
                .ok = true,
                .userMessage = QStringLiteral("Rumble account %1 was added.")
                                   .arg(username),
                .account = std::move(account),
            });
        });
}

void RumbleAccountManager::removeAccount(RumbleAccount *account)
{
    if (!account)
    {
        return;
    }
    const auto userID = account->userID();
    account->clearCredential();
    if (this->currentAccountID.getValue() == userID)
    {
        this->currentAccountID = "";
        if (this->current_)
        {
            this->refreshCurrent();
        }
    }

    const auto basePath = accountSettingsPath(userID);
    pajlada::Settings::SettingManager::gRemoveSetting(basePath);
    this->credentialStore_->erase(
        this->credentialKey(userID),
        [lifetime = std::weak_ptr<void>(this->lifetime_),
         signal = &this->storageError](rumble::CredentialStoreError error) {
            if (lifetime.expired() ||
                error == rumble::CredentialStoreError::None)
            {
                return;
            }
            signal->invoke(rumble::credentialStoreErrorText(error));
        });
    this->userListUpdated.invoke();
    if (auto *settings = getSettings())
    {
        std::ignore = settings->requestSave();
    }
}

}  // namespace chatterino
