// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/CompactTabControls.hpp"

#include "widgets/buttons/LabelButton.hpp"

#include <QSizePolicy>
#include <QtGlobal>

namespace chatterino {

namespace {

QString displayTitle(const CompactTabStatus &status,
                     const QString &selectedTitle)
{
    if (status.selectedPosition == 0)
    {
        return QStringLiteral("No selected tab");
    }
    if (selectedTitle.isEmpty())
    {
        return QStringLiteral("Untitled tab");
    }
    return selectedTitle;
}

}  // namespace

QString compactTabStatusText(const CompactTabStatus &status)
{
    return QStringLiteral("%1 / %2 tabs")
        .arg(static_cast<qulonglong>(status.selectedPosition))
        .arg(static_cast<qulonglong>(status.navigableCount));
}

void initializeCompactTabControlButtons(const CompactTabControlButtons &buttons)
{
    Q_ASSERT(buttons.complete());

    buttons.previousLive->setObjectName(
        QStringLiteral("compactTabPreviousLive"));
    buttons.previous->setObjectName(QStringLiteral("compactTabPrevious"));
    buttons.status->setObjectName(QStringLiteral("compactTabStatus"));
    buttons.next->setObjectName(QStringLiteral("compactTabNext"));
    buttons.nextLive->setObjectName(QStringLiteral("compactTabNextLive"));

    buttons.previousLive->setText(QString(QChar(0x2039)));
    buttons.previous->setText(QString(QChar(0x2039)));
    buttons.next->setText(QString(QChar(0x203A)));
    buttons.nextLive->setText(QString(QChar(0x203A)));
    buttons.previousLive->useTabLiveIndicatorColor();
    buttons.nextLive->useTabLiveIndicatorColor();
    buttons.previousLive->setPadding({0, 0});
    buttons.previous->setPadding({0, 0});
    buttons.status->setPadding({4, 0});
    buttons.next->setPadding({0, 0});
    buttons.nextLive->setPadding({0, 0});

    buttons.status->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    buttons.status->setCursor(Qt::PointingHandCursor);

    for (auto *button : {buttons.previousLive, buttons.previous,
                         buttons.status, buttons.next, buttons.nextLive})
    {
        button->setFocusPolicy(Qt::NoFocus);
    }
}

void applyCompactTabControlState(const CompactTabControlButtons &buttons,
                                 const CompactTabStatus &status,
                                 const QString &selectedTitle, bool visible)
{
    if (!buttons.complete())
    {
        return;
    }

    const auto statusText = compactTabStatusText(status);
    const auto title = displayTitle(status, selectedTitle);
    const auto currentTab = QStringLiteral("Current tab: %1").arg(title);

    buttons.status->setText(statusText);
    buttons.status->updateGeometry();
    buttons.status->setToolTip(
        QStringLiteral("Show tabs — %1 — %2").arg(statusText, currentTab));
    buttons.status->setAccessibleName(
        QStringLiteral("Show tabs, %1, selected tab: %2")
            .arg(statusText, title));
    buttons.status->setAccessibleDescription(
        QStringLiteral(
            "Temporarily reveal the normal tab strip for direct selection. %1")
            .arg(currentTab));

    buttons.previous->setToolTip(
        QStringLiteral("Previous tab — %1").arg(currentTab));
    buttons.previous->setAccessibleName(
        QStringLiteral("Previous tab, current tab: %1").arg(title));
    buttons.previous->setAccessibleDescription(
        QStringLiteral("Select the previous navigable tab with wraparound. %1")
            .arg(currentTab));

    buttons.previousLive->setToolTip(
        QStringLiteral("Previous live tab — %1").arg(currentTab));
    buttons.previousLive->setAccessibleName(
        QStringLiteral("Previous live tab, current tab: %1").arg(title));
    buttons.previousLive->setAccessibleDescription(
        QStringLiteral(
            "Select the previous visible live tab with wraparound. %1")
            .arg(currentTab));

    buttons.next->setToolTip(QStringLiteral("Next tab — %1").arg(currentTab));
    buttons.next->setAccessibleName(
        QStringLiteral("Next tab, current tab: %1").arg(title));
    buttons.next->setAccessibleDescription(
        QStringLiteral("Select the next navigable tab with wraparound. %1")
            .arg(currentTab));

    buttons.nextLive->setToolTip(
        QStringLiteral("Next live tab — %1").arg(currentTab));
    buttons.nextLive->setAccessibleName(
        QStringLiteral("Next live tab, current tab: %1").arg(title));
    buttons.nextLive->setAccessibleDescription(
        QStringLiteral("Select the next visible live tab with wraparound. %1")
            .arg(currentTab));

    const bool controlsEnabled = visible && status.controlsEnabled;
    buttons.previous->setEnabled(controlsEnabled);
    buttons.next->setEnabled(controlsEnabled);
    static_cast<QWidget *>(buttons.previous)->setEnabled(controlsEnabled);
    static_cast<QWidget *>(buttons.next)->setEnabled(controlsEnabled);
    buttons.status->setEnabled(visible);
    static_cast<QWidget *>(buttons.status)->setEnabled(visible);

    const bool liveControlsEnabled =
        visible && status.liveControlsEnabled;
    buttons.previousLive->setEnabled(liveControlsEnabled);
    buttons.nextLive->setEnabled(liveControlsEnabled);
    static_cast<QWidget *>(buttons.previousLive)
        ->setEnabled(liveControlsEnabled);
    static_cast<QWidget *>(buttons.nextLive)->setEnabled(liveControlsEnabled);

    buttons.previousLive->setVisible(visible);
    buttons.previous->setVisible(visible);
    buttons.status->setVisible(visible);
    buttons.next->setVisible(visible);
    buttons.nextLive->setVisible(visible);
}

}  // namespace chatterino
