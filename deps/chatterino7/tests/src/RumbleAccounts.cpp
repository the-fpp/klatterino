// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleAccount.hpp"
#include "providers/rumble/RumbleAccountManager.hpp"
#include "providers/rumble/RumbleCredentialStore.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <gtest/gtest.h>
#include <pajlada/settings/settingmanager.hpp>
#include <QMap>

#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

using namespace chatterino;

namespace {

struct CredentialBackend {
    QMap<QString, QByteArray> values;
    QString lastReadKey;
};

struct DeferredCredentialBackend {
    rumble::CredentialStore::WriteCallback pendingWrite;
};

class FakeCredentialStore final : public rumble::CredentialStore
{
public:
    explicit FakeCredentialStore(std::shared_ptr<CredentialBackend> backend,
                                 bool available = true)
        : backend_(std::move(backend))
        , available_(available)
    {
    }

    bool available() const override
    {
        return this->available_;
    }

    void read(QString key, ReadCallback callback) override
    {
        this->backend_->lastReadKey = key;
        const auto it = this->backend_->values.constFind(key);
        if (it == this->backend_->values.cend())
        {
            callback({
                .error = rumble::CredentialStoreError::NotFound,
            });
            return;
        }
        callback({
            .error = rumble::CredentialStoreError::None,
            .secret = *it,
        });
    }

    void write(QString key, QByteArray secret, WriteCallback callback) override
    {
        this->backend_->values.insert(std::move(key), std::move(secret));
        callback(rumble::CredentialStoreError::None);
    }

    void erase(QString key, WriteCallback callback) override
    {
        this->backend_->values.remove(key);
        callback(rumble::CredentialStoreError::None);
    }

private:
    std::shared_ptr<CredentialBackend> backend_;
    bool available_ = true;
};

class DeferredCredentialStore final : public rumble::CredentialStore
{
public:
    explicit DeferredCredentialStore(
        std::shared_ptr<DeferredCredentialBackend> backend)
        : backend_(std::move(backend))
    {
    }

    bool available() const override
    {
        return true;
    }

    void read(QString, ReadCallback callback) override
    {
        callback({.error = rumble::CredentialStoreError::NotFound});
    }

    void write(QString, QByteArray secret, WriteCallback callback) override
    {
        secret.fill('\0');
        secret.clear();
        this->backend_->pendingWrite = std::move(callback);
    }

    void erase(QString, WriteCallback callback) override
    {
        callback(rumble::CredentialStoreError::None);
    }

private:
    std::shared_ptr<DeferredCredentialBackend> backend_;
};

void clearRumbleAccountSettings()
{
    pajlada::Settings::SettingManager::gRemoveSetting("/rumbleAccounts");
}

}  // namespace

TEST(RumbleAccounts, SecureStoreAndMetadataSurviveManagerRestart)
{
    clearRumbleAccountSettings();
    auto backend = std::make_shared<CredentialBackend>();
    const auto userID = QStringLiteral("123/unsafe-settings-path");
    const auto username = QStringLiteral("Synthetic Rumble User");
    const auto secret = QByteArrayLiteral("SYNTHETIC_SESSION_CANARY");

    {
        RumbleAccountManager manager(
            nullptr, std::make_unique<FakeCredentialStore>(backend));
        std::optional<RumbleAccountAddResult> added;
        manager.addValidatedAccount(userID, username, secret,
                                    [&](RumbleAccountAddResult result) {
                                        added = std::move(result);
                                    });

        ASSERT_TRUE(added);
        ASSERT_TRUE(added->ok);
        ASSERT_TRUE(added->account);
        EXPECT_EQ(added->account->userID(), userID);
        EXPECT_EQ(added->account->username(), username);
        EXPECT_EQ(manager.accountList().size(), 1U);
        EXPECT_EQ(manager.currentAccountID.getValue(), userID);
        ASSERT_TRUE(manager.current());
        EXPECT_EQ(manager.current()->userID(), userID);
        ASSERT_TRUE(manager.currentCredential());
        EXPECT_EQ(*manager.currentCredential(), secret);

        ASSERT_EQ(backend->values.size(), 1);
        EXPECT_FALSE(backend->values.firstKey().contains(userID));
        const auto keys =
            pajlada::Settings::SettingManager::getObjectKeys("/rumbleAccounts");
        ASSERT_EQ(keys.size(), 2);
        EXPECT_TRUE(std::ranges::any_of(keys, [](const auto &key) {
            return key == "current";
        }));
        EXPECT_TRUE(std::ranges::any_of(keys, [&](const auto &key) {
            return key.starts_with("uid") &&
                   key.find(userID.toStdString()) == std::string::npos;
        }));
    }

    ASSERT_EQ(backend->values.size(), 1);
    EXPECT_EQ(backend->values.first(), secret);

    {
        RumbleAccountManager manager(
            nullptr, std::make_unique<FakeCredentialStore>(backend));
        manager.load();

        ASSERT_EQ(manager.accountList().size(), 1);
        ASSERT_TRUE(manager.current());
        EXPECT_EQ(manager.current()->userID(), userID);
        EXPECT_EQ(manager.current()->username(), username);
        EXPECT_EQ(backend->lastReadKey, backend->values.firstKey());
        EXPECT_TRUE(manager.current()->ready());
        ASSERT_TRUE(manager.currentCredential());
        EXPECT_EQ(*manager.currentCredential(), secret);

        manager.accounts.removeFirstMatching([&](const auto &account) {
            return account->userID() == userID;
        });
        EXPECT_TRUE(manager.accountList().empty());
        EXPECT_TRUE(backend->values.empty());
        EXPECT_FALSE(manager.current());
    }

    clearRumbleAccountSettings();
}

TEST(RumbleAccounts, RefusesAccountWhenSecureStorageIsUnavailable)
{
    clearRumbleAccountSettings();
    auto backend = std::make_shared<CredentialBackend>();
    RumbleAccountManager manager(
        nullptr, std::make_unique<FakeCredentialStore>(backend, false));

    std::optional<RumbleAccountAddResult> added;
    manager.addValidatedAccount(QStringLiteral("42"),
                                QStringLiteral("Synthetic User"),
                                QByteArrayLiteral("SYNTHETIC_SESSION_CANARY"),
                                [&](RumbleAccountAddResult result) {
                                    added = std::move(result);
                                });

    ASSERT_TRUE(added);
    EXPECT_FALSE(added->ok);
    EXPECT_TRUE(manager.accountList().empty());
    EXPECT_TRUE(backend->values.empty());
    clearRumbleAccountSettings();
}

TEST(RumbleAccounts, ReplacesAndRemovesOneAccountWithoutPlaintextSecretMetadata)
{
    clearRumbleAccountSettings();
    auto backend = std::make_shared<CredentialBackend>();
    RumbleAccountManager manager(
        nullptr, std::make_unique<FakeCredentialStore>(backend));

    std::optional<RumbleAccountAddResult> first;
    manager.addValidatedAccount(QStringLiteral("42"),
                                QStringLiteral("First Name"),
                                QByteArrayLiteral("FIRST_SYNTHETIC_SESSION"),
                                [&](RumbleAccountAddResult result) {
                                    first = std::move(result);
                                });
    ASSERT_TRUE(first && first->ok && first->account);

    std::optional<RumbleAccountAddResult> replacement;
    manager.addValidatedAccount(
        QStringLiteral("42"), QStringLiteral("Updated Name"),
        QByteArrayLiteral("REPLACEMENT_SYNTHETIC_SESSION"),
        [&](RumbleAccountAddResult result) {
            replacement = std::move(result);
        });
    ASSERT_TRUE(replacement && replacement->ok && replacement->account);
    EXPECT_EQ(replacement->account, first->account);
    ASSERT_EQ(manager.accountList().size(), 1U);
    EXPECT_EQ(manager.current()->username(), QStringLiteral("Updated Name"));
    EXPECT_EQ(manager.currentCredential(),
              QByteArrayLiteral("REPLACEMENT_SYNTHETIC_SESSION"));
    ASSERT_EQ(backend->values.size(), 1);
    EXPECT_EQ(backend->values.first(),
              QByteArrayLiteral("REPLACEMENT_SYNTHETIC_SESSION"));

    const auto keys =
        pajlada::Settings::SettingManager::getObjectKeys("/rumbleAccounts");
    for (const auto &key : keys)
    {
        const auto base = "/rumbleAccounts/" + key;
        EXPECT_NE(pajlada::Settings::Setting<QString>::get(base + "/userID"),
                  QStringLiteral("REPLACEMENT_SYNTHETIC_SESSION"));
        EXPECT_NE(pajlada::Settings::Setting<QString>::get(base + "/username"),
                  QStringLiteral("REPLACEMENT_SYNTHETIC_SESSION"));
    }

    manager.accounts.removeFirstMatching([](const auto &) {
        return true;
    });
    EXPECT_TRUE(manager.accountList().empty());
    EXPECT_TRUE(backend->values.empty());
    EXPECT_FALSE(manager.current());
    clearRumbleAccountSettings();
}

TEST(RumbleAccounts, DropsDeferredCredentialCallbackAfterManagerDestruction)
{
    clearRumbleAccountSettings();
    auto backend = std::make_shared<DeferredCredentialBackend>();
    bool callbackCalled = false;
    {
        RumbleAccountManager manager(
            nullptr, std::make_unique<DeferredCredentialStore>(backend));
        manager.addValidatedAccount(
            QStringLiteral("42"), QStringLiteral("Synthetic User"),
            QByteArrayLiteral("SYNTHETIC_SESSION_CANARY"),
            [&](RumbleAccountAddResult) {
                callbackCalled = true;
            });
        ASSERT_TRUE(backend->pendingWrite);
    }

    auto completion = std::move(backend->pendingWrite);
    completion(rumble::CredentialStoreError::None);
    EXPECT_FALSE(callbackCalled);
    clearRumbleAccountSettings();
}
