// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVector.hpp"
#include "providers/rumble/RumbleCredentialStore.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QObject;

namespace chatterino {

class RumbleAccount;

struct RumbleAccountAddResult {
    bool ok = false;
    QString userMessage;
    std::shared_ptr<RumbleAccount> account;
};

/// Owns non-secret Rumble account metadata and asynchronously loads each
/// password-equivalent session from the platform credential store.
class RumbleAccountManager
{
public:
    using AddCallback = std::function<void(RumbleAccountAddResult)>;

    explicit RumbleAccountManager(
        QObject *owner = nullptr,
        std::unique_ptr<rumble::CredentialStore> credentialStore = {});
    ~RumbleAccountManager();

    RumbleAccountManager(const RumbleAccountManager &) = delete;
    RumbleAccountManager &operator=(const RumbleAccountManager &) = delete;

    void load();
    void addValidatedAccount(QString userID, QString username,
                             QByteArray credential, AddCallback callback);
    void selectAccount(const QString &userID);

    [[nodiscard]] bool secureStorageAvailable() const;
    [[nodiscard]] std::shared_ptr<RumbleAccount> current() const;
    [[nodiscard]] std::shared_ptr<RumbleAccount> findByID(
        const QString &userID) const;
    [[nodiscard]] std::vector<std::shared_ptr<RumbleAccount>> accountList()
        const;
    [[nodiscard]] std::optional<QByteArray> currentCredential() const;

    pajlada::Settings::Setting<QString> currentAccountID{
        "/rumbleAccounts/current", ""};
    pajlada::Signals::NoArgSignal currentUserChanged;
    pajlada::Signals::NoArgSignal userListUpdated;
    pajlada::Signals::Signal<QString> storageError;
    SignalVector<std::shared_ptr<RumbleAccount>> accounts;

private:
    static bool validIdentity(const QString &userID, const QString &username);
    static QString credentialKey(const QString &userID);
    void removeAccount(RumbleAccount *account);
    void refreshCurrent();

    std::unique_ptr<rumble::CredentialStore> credentialStore_;
    std::shared_ptr<RumbleAccount> current_;
    std::shared_ptr<void> lifetime_;
    bool loaded_ = false;
};

}  // namespace chatterino
