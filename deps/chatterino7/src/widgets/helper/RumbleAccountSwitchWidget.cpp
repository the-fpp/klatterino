// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "widgets/helper/RumbleAccountSwitchWidget.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/rumble/RumbleAccount.hpp"

#include <QSignalBlocker>

namespace chatterino {

RumbleAccountSwitchWidget::RumbleAccountSwitchWidget(QWidget *parent)
    : QListWidget(parent)
{
    auto &manager = getApp()->getAccounts()->rumble;
    this->managedConnections_.managedConnect(manager.userListUpdated, [this] {
        this->refreshItems();
        this->refresh();
    });
    this->managedConnections_.managedConnect(manager.currentUserChanged,
                                             [this] {
                                                 this->refresh();
                                             });

    this->refreshItems();
    this->refresh();

    QObject::connect(this, &QListWidget::clicked, this, [this] {
        if (const auto *item = this->currentItem())
        {
            getApp()->getAccounts()->rumble.selectAccount(
                item->data(Qt::UserRole).toString());
        }
    });
}

void RumbleAccountSwitchWidget::refresh()
{
    QSignalBlocker blocker(this);
    const auto current = getApp()->getAccounts()->rumble.current();
    const auto currentID = current ? current->userID() : QString{};
    for (int i = 0; i < this->count(); ++i)
    {
        if (this->item(i)->data(Qt::UserRole).toString() == currentID)
        {
            this->setCurrentRow(i);
            return;
        }
    }
    if (this->count() > 0)
    {
        this->setCurrentRow(0);
    }
}

void RumbleAccountSwitchWidget::refreshItems()
{
    QSignalBlocker blocker(this);
    this->clear();
    auto *anonymous = new QListWidgetItem(ANONYMOUS_USERNAME_LABEL, this);
    anonymous->setData(Qt::UserRole, QString{});

    for (const auto &account : getApp()->getAccounts()->rumble.accountList())
    {
        auto *item = new QListWidgetItem(account->username(), this);
        item->setData(Qt::UserRole, account->userID());
        if (!account->ready())
        {
            item->setToolTip(
                QStringLiteral("Secure sign-in is not currently available."));
        }
    }
}

}  // namespace chatterino
