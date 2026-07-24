// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/accounts/AccountModel.hpp"

#include "controllers/accounts/Account.hpp"
#include "util/SharedPtrElementLess.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <vector>

namespace chatterino {
namespace {

class TestAccount final : public Account
{
public:
    TestAccount(ProviderId provider, QString name)
        : Account(provider)
        , name_(std::move(name))
    {
    }

    QString toString() const override
    {
        return this->name_;
    }

private:
    QString name_;
};

using AccountPtr = std::shared_ptr<Account>;
using AccountVector = SignalVector<AccountPtr>;

AccountPtr account(ProviderId provider, QString name)
{
    return std::make_shared<TestAccount>(provider, std::move(name));
}

QString providerName(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Twitch:
            return QStringLiteral("Twitch");
        case ProviderId::Kick:
            return QStringLiteral("Kick");
        case ProviderId::Rumble:
            return QStringLiteral("Rumble");
    }
    return {};
}

QStringList rows(const AccountModel &model)
{
    QStringList result;
    for (int row = 0; row < model.rowCount(QModelIndex()); ++row)
    {
        const auto index = model.index(row, 0);
        const auto prefix = model.flags(index).testFlag(Qt::ItemIsSelectable)
                                ? QStringLiteral("account:")
                                : QStringLiteral("heading:");
        result.append(prefix + model.data(index, Qt::DisplayRole).toString());
    }
    return result;
}

QStringList expectedSingleAccountRows()
{
    return {
        QStringLiteral("heading:Kick"),   QStringLiteral("account:kick-user"),
        QStringLiteral("heading:Rumble"), QStringLiteral("account:rumble-user"),
        QStringLiteral("heading:Twitch"), QStringLiteral("account:twitch-user"),
    };
}

std::array<AccountPtr, 3> onePerProvider()
{
    return {
        account(ProviderId::Twitch, QStringLiteral("twitch-user")),
        account(ProviderId::Kick, QStringLiteral("kick-user")),
        account(ProviderId::Rumble, QStringLiteral("rumble-user")),
    };
}

std::vector<std::array<int, 3>> insertionOrders()
{
    return {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
}

TEST(AccountModel, GroupsDynamicProvidersForEveryInsertionOrder)
{
    for (const auto &order : insertionOrders())
    {
        SCOPED_TRACE(testing::Message() << order[0] << order[1] << order[2]);
        AccountVector accounts(SharedPtrElementLess<Account>{});
        AccountModel model(nullptr);
        model.initialize(&accounts);
        const auto items = onePerProvider();

        for (const auto index : order)
        {
            accounts.insert(items[index]);
        }

        EXPECT_EQ(rows(model), expectedSingleAccountRows());
    }
}

TEST(AccountModel, GroupsPreloadedProvidersForEveryDataInsertionOrder)
{
    for (const auto &order : insertionOrders())
    {
        SCOPED_TRACE(testing::Message() << order[0] << order[1] << order[2]);
        AccountVector accounts(SharedPtrElementLess<Account>{});
        const auto items = onePerProvider();
        for (const auto index : order)
        {
            accounts.insert(items[index]);
        }

        AccountModel model(nullptr);
        model.initialize(&accounts);
        EXPECT_EQ(rows(model), expectedSingleAccountRows());
    }
}

TEST(AccountModel, PreservesProviderIdentityForEqualNamesAndRemoval)
{
    AccountVector accounts(SharedPtrElementLess<Account>{});
    AccountModel model(nullptr);
    model.initialize(&accounts);

    for (const auto provider :
         {ProviderId::Twitch, ProviderId::Kick, ProviderId::Rumble})
    {
        accounts.insert(account(provider, QStringLiteral("same-user")));
    }

    EXPECT_EQ(rows(model), QStringList({
                               QStringLiteral("heading:Kick"),
                               QStringLiteral("account:same-user"),
                               QStringLiteral("heading:Rumble"),
                               QStringLiteral("account:same-user"),
                               QStringLiteral("heading:Twitch"),
                               QStringLiteral("account:same-user"),
                           }));
    for (int row = 0; row < model.rowCount(QModelIndex()); ++row)
    {
        const auto index = model.index(row, 0);
        if (!model.flags(index).testFlag(Qt::ItemIsSelectable))
        {
            continue;
        }
        const auto heading =
            model.data(model.index(row - 1, 0), Qt::DisplayRole).toString();
        const auto provider = static_cast<ProviderId>(
            model.data(index, AccountModel::ProviderIdRole).toInt());
        EXPECT_EQ(providerName(provider), heading);
    }

    ASSERT_TRUE(model.removeRow(3));
    ASSERT_EQ(accounts.raw().size(), 2U);
    EXPECT_TRUE(std::ranges::none_of(accounts.raw(), [](const auto &item) {
        return item->getProviderId() == ProviderId::Rumble;
    }));
    EXPECT_EQ(rows(model), QStringList({
                               QStringLiteral("heading:Kick"),
                               QStringLiteral("account:same-user"),
                               QStringLiteral("heading:Twitch"),
                               QStringLiteral("account:same-user"),
                           }));
}

TEST(AccountModel, SortsMultipleAccountsAndRepeatedlyAddsAndRemovesHeadings)
{
    AccountVector accounts(SharedPtrElementLess<Account>{});
    AccountModel model(nullptr);
    model.initialize(&accounts);

    const auto kickZed = account(ProviderId::Kick, QStringLiteral("zed"));
    const auto kickAlpha = account(ProviderId::Kick, QStringLiteral("alpha"));
    const auto rumble = account(ProviderId::Rumble, QStringLiteral("rumble"));
    const auto twitch = account(ProviderId::Twitch, QStringLiteral("twitch"));
    accounts.insert(twitch);
    accounts.insert(kickZed);
    accounts.insert(rumble);
    accounts.insert(kickAlpha);

    EXPECT_EQ(rows(model), QStringList({
                               QStringLiteral("heading:Kick"),
                               QStringLiteral("account:alpha"),
                               QStringLiteral("account:zed"),
                               QStringLiteral("heading:Rumble"),
                               QStringLiteral("account:rumble"),
                               QStringLiteral("heading:Twitch"),
                               QStringLiteral("account:twitch"),
                           }));

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        ASSERT_TRUE(accounts.removeFirstMatching([](const auto &item) {
            return item->getProviderId() == ProviderId::Rumble;
        }));
        EXPECT_FALSE(rows(model).contains(QStringLiteral("heading:Rumble")));

        accounts.insert(account(ProviderId::Rumble,
                                QStringLiteral("rumble-%1").arg(cycle)));
        EXPECT_EQ(rows(model).at(3), QStringLiteral("heading:Rumble"));
        EXPECT_EQ(rows(model).at(4),
                  QStringLiteral("account:rumble-%1").arg(cycle));
    }

    ASSERT_TRUE(accounts.removeFirstMatching([&](const auto &item) {
        return item == kickAlpha;
    }));
    EXPECT_TRUE(rows(model).contains(QStringLiteral("heading:Kick")));
    ASSERT_TRUE(accounts.removeFirstMatching([&](const auto &item) {
        return item == kickZed;
    }));
    EXPECT_FALSE(rows(model).contains(QStringLiteral("heading:Kick")));
    EXPECT_TRUE(rows(model).contains(QStringLiteral("heading:Rumble")));
    EXPECT_TRUE(rows(model).contains(QStringLiteral("heading:Twitch")));
}

TEST(AccountModel, ModelRemovalKeepsRemainingProviderRowsIntact)
{
    AccountVector accounts(SharedPtrElementLess<Account>{});
    AccountModel model(nullptr);
    model.initialize(&accounts);

    accounts.insert(account(ProviderId::Kick, QStringLiteral("same-user")));
    accounts.insert(account(ProviderId::Kick, QStringLiteral("second-kick")));
    accounts.insert(account(ProviderId::Twitch, QStringLiteral("same-user")));

    ASSERT_TRUE(model.removeRow(2));
    EXPECT_EQ(rows(model), QStringList({
                               QStringLiteral("heading:Kick"),
                               QStringLiteral("account:same-user"),
                               QStringLiteral("heading:Twitch"),
                               QStringLiteral("account:same-user"),
                           }));

    ASSERT_TRUE(model.removeRow(1));
    EXPECT_EQ(rows(model), QStringList({
                               QStringLiteral("heading:Twitch"),
                               QStringLiteral("account:same-user"),
                           }));
}

}  // namespace
}  // namespace chatterino
