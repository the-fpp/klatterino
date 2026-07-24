// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QListWidget>

namespace chatterino {

class RumbleAccountSwitchWidget : public QListWidget
{
public:
    explicit RumbleAccountSwitchWidget(QWidget *parent = nullptr);

    void refresh();

private:
    void refreshItems();

    pajlada::Signals::SignalHolder managedConnections_;
};

}  // namespace chatterino
