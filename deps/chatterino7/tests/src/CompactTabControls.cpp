// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/hotkeys/ActionNames.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "mocks/BaseApplication.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QTimer>
#include <QWindow>

#include <array>
#include <utility>

namespace chatterino {

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args_, this->paths_, this->settings, this->theme,
                        this->fonts)
    {
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    HotkeyController hotkeys;
    WindowManager windowManager;
};

class TestNotebook : public Notebook
{
public:
    explicit TestNotebook(QWidget *parent)
        : Notebook(parent)
    {
        auto *profile =
            this->addCustomButton<LabelButton>(QStringLiteral("Profile"));
        profile->setObjectName(QStringLiteral("profileControl"));
        this->addCompactTabControls();
        this->setShowAddButton(true);
    }

    void setUserTabsVisible(bool visible)
    {
        this->setShowTabs(visible);
    }

    bool userTabsVisible() const
    {
        return this->getShowTabs();
    }

    void setVisibilityFilter(TabVisibilityFilter filter)
    {
        this->setTabVisibilityFilter(std::move(filter));
    }

    DrawnButton *addTabButton() const
    {
        return this->addButton_;
    }
};

class CompactTabControlsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        this->mockApplication.settings.informOnTabVisibilityToggle.setValue(
            false);
        this->host.resize(1200, 700);
        this->notebook.setGeometry(this->host.rect());
        this->host.show();
        this->notebook.show();
        QCoreApplication::processEvents();
    }

    LabelButton *previous() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("compactTabPrevious")));
    }

    LabelButton *previousLive() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("compactTabPreviousLive")));
    }

    LabelButton *profile() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("profileControl")));
    }

    LabelButton *status() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("compactTabStatus")));
    }

    LabelButton *next() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("compactTabNext")));
    }

    LabelButton *nextLive() const
    {
        return static_cast<LabelButton *>(this->notebook.findChild<QWidget *>(
            QStringLiteral("compactTabNextLive")));
    }

    MockApplication mockApplication;
    BaseWindow host;
    TestNotebook notebook{&this->host};
};

class ResponsiveTabsRegressionTest : public CompactTabControlsTest
{
};

void click(QWidget *widget)
{
    ASSERT_NE(widget, nullptr);
    const auto local = QPointF(widget->rect().center());
    const auto global = QPointF(widget->mapToGlobal(local.toPoint()));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    QCoreApplication::processEvents();
}

}  // namespace

TEST_F(CompactTabControlsTest, ZeroOneAndManyStatesControlEnablement)
{
    ASSERT_NE(this->previous(), nullptr);
    ASSERT_NE(this->previousLive(), nullptr);
    ASSERT_NE(this->status(), nullptr);
    ASSERT_NE(this->next(), nullptr);
    ASSERT_NE(this->nextLive(), nullptr);

    this->notebook.setCompactMode(true);
    EXPECT_EQ(this->status()->text(), QStringLiteral("0 / 0 tabs"));
    EXPECT_FALSE(this->previous()->enabled());
    EXPECT_FALSE(this->next()->enabled());
    EXPECT_FALSE(this->previous()->isEnabled());
    EXPECT_FALSE(this->next()->isEnabled());
    EXPECT_FALSE(this->previousLive()->enabled());
    EXPECT_FALSE(this->nextLive()->enabled());

    this->notebook.setVisibilityFilter([](const NotebookTab *) {
        return true;
    });
    this->notebook.selectPreviousTab();
    this->notebook.selectNextTab();
    EXPECT_EQ(this->notebook.getSelectedPage(), nullptr);

    this->notebook.addPage(new QWidget(), QStringLiteral("First"), true);
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 1 tabs"));
    EXPECT_FALSE(this->previous()->enabled());
    EXPECT_FALSE(this->next()->enabled());
    EXPECT_FALSE(this->previousLive()->enabled());
    EXPECT_FALSE(this->nextLive()->enabled());

    this->notebook.addPage(new QWidget(), QStringLiteral("Second"));
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 2 tabs"));
    EXPECT_TRUE(this->previous()->enabled());
    EXPECT_TRUE(this->next()->enabled());
    EXPECT_TRUE(this->previous()->isEnabled());
    EXPECT_TRUE(this->next()->isEnabled());
    EXPECT_FALSE(this->previousLive()->enabled());
    EXPECT_FALSE(this->nextLive()->enabled());
}

TEST_F(CompactTabControlsTest, LiveCycleHotkeyActionsAreConfigurable)
{
    const auto &windowActions = actionNames.at(HotkeyCategory::Window);
    EXPECT_TRUE(
        windowActions.contains(QStringLiteral("selectPreviousLiveTab")));
    EXPECT_TRUE(windowActions.contains(QStringLiteral("selectNextLiveTab")));
    EXPECT_EQ(windowActions.at(QStringLiteral("selectPreviousLiveTab"))
                  .minCountArguments,
              0);
    EXPECT_EQ(windowActions.at(QStringLiteral("selectNextLiveTab"))
                  .minCountArguments,
              0);
}

TEST_F(CompactTabControlsTest,
       LiveControlsUseAggregateStateOrderFilteringAndWrap)
{
    auto *firstPage = new QWidget();
    auto *offlinePage = new QWidget();
    auto *thirdPage = new QWidget();
    auto *fourthPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *offlineTab =
        this->notebook.addPage(offlinePage, QStringLiteral("Offline"));
    auto *thirdTab =
        this->notebook.addPage(thirdPage, QStringLiteral("Third"));
    auto *fourthTab =
        this->notebook.addPage(fourthPage, QStringLiteral("Fourth"));
    firstTab->setLive(true);
    thirdTab->setLive(true);
    fourthTab->setLive(true);
    this->notebook.setCompactMode(true);
    this->notebook.refresh();

    ASSERT_TRUE(this->previousLive()->enabled());
    ASSERT_TRUE(this->nextLive()->enabled());
    EXPECT_TRUE(this->nextLive()->toolTip().contains(QStringLiteral("live")));
    EXPECT_TRUE(this->previousLive()->accessibleName().contains(
        QStringLiteral("Previous live tab")));

    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), thirdPage);
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), fourthPage);
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);
    Q_EMIT this->previousLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), fourthPage);

    // The normal arrows keep the existing all-navigable traversal.
    this->notebook.select(firstPage);
    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), offlinePage);
    EXPECT_FALSE(offlineTab->isLive());

    // Reordering changes the live sequence without caching a second order.
    this->notebook.rearrangePage(fourthPage, 1);
    this->notebook.select(firstPage);
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), fourthPage);

    // Visibility eligibility is shared with normal compact traversal.
    this->notebook.select(firstPage);
    this->notebook.setVisibilityFilter([thirdTab](const NotebookTab *tab) {
        return tab != thirdTab;
    });
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), fourthPage);
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);

    // Remove/filter/state changes update enablement but do not move selection.
    const auto *selected = this->notebook.getSelectedPage();
    this->notebook.removePage(fourthPage);
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);
    EXPECT_FALSE(this->previousLive()->enabled());
    EXPECT_FALSE(this->nextLive()->enabled());

    this->notebook.setVisibilityFilter({});
    ASSERT_TRUE(this->nextLive()->enabled());
    thirdTab->setLive(false);
    this->notebook.refresh();
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);
    EXPECT_FALSE(this->previousLive()->enabled());
    EXPECT_FALSE(this->nextLive()->enabled());
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);
}

TEST_F(CompactTabControlsTest, ModeTransitionsPreserveUserStateAndSelection)
{
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));

    ASSERT_FALSE(firstTab->isHidden());
    ASSERT_FALSE(secondTab->isHidden());
    ASSERT_TRUE(this->previous()->isHidden());
    auto *selected = this->notebook.getSelectedPage();

    this->notebook.setCompactMode(true);
    EXPECT_TRUE(firstTab->isHidden());
    EXPECT_TRUE(secondTab->isHidden());
    EXPECT_FALSE(this->previous()->isHidden());
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);
    EXPECT_TRUE(this->notebook.userTabsVisible());

    this->notebook.setCompactMode(false);
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_TRUE(this->previous()->isHidden());
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);

    this->notebook.setUserTabsVisible(false);
    ASSERT_FALSE(this->notebook.userTabsVisible());
    ASSERT_TRUE(firstTab->isHidden());
    ASSERT_TRUE(secondTab->isHidden());

    this->notebook.setCompactMode(true);
    this->notebook.setCompactMode(false);
    EXPECT_FALSE(this->notebook.userTabsVisible());
    EXPECT_TRUE(firstTab->isHidden());
    EXPECT_TRUE(secondTab->isHidden());
    EXPECT_EQ(this->notebook.getSelectedPage(), selected);
}

TEST_F(CompactTabControlsTest,
       StatusRevealsTabsAndOutsideClickRestoresCompactControls)
{
    auto *firstPage = new QWidget();
    auto *firstInput = new QLineEdit(firstPage);
    firstInput->setGeometry(0, 0, 200, 30);
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(new QWidget(), QStringLiteral("Second"));
    this->notebook.setCompactMode(true);
    firstInput->setFocus();
    QCoreApplication::processEvents();

    ASSERT_TRUE(firstTab->isHidden());
    ASSERT_TRUE(secondTab->isHidden());
    ASSERT_FALSE(this->status()->isHidden());
    EXPECT_TRUE(this->status()->enabled());
    EXPECT_TRUE(this->status()->isEnabled());
    EXPECT_EQ(this->status()->cursor().shape(), Qt::PointingHandCursor);
    EXPECT_TRUE(
        this->status()->toolTip().startsWith(QStringLiteral("Show tabs")));
    EXPECT_TRUE(this->status()->accessibleName().startsWith(
        QStringLiteral("Show tabs")));
    EXPECT_TRUE(this->status()->accessibleDescription().contains(
        QStringLiteral("direct selection")));

    Q_EMIT this->status()->leftClicked();

    EXPECT_TRUE(this->notebook.isCompactMode());
    EXPECT_TRUE(this->notebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_TRUE(this->status()->isHidden());
    EXPECT_TRUE(firstInput->hasFocus());

    click(this->profile());

    EXPECT_FALSE(this->notebook.areCompactTabsRevealed());
    EXPECT_TRUE(firstTab->isHidden());
    EXPECT_TRUE(secondTab->isHidden());
    EXPECT_FALSE(this->status()->isHidden());
}

TEST_F(CompactTabControlsTest,
       DirectSelectionKeepsRevealAndRestoresDestinationFocus)
{
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstInput = new QLineEdit(firstPage);
    auto *secondInput = new QLineEdit(secondPage);
    firstInput->setGeometry(0, 0, 200, 30);
    secondInput->setGeometry(0, 0, 200, 30);
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));

    this->notebook.select(secondPage);
    secondInput->setFocus();
    this->notebook.select(firstPage, false);
    firstInput->setFocus();
    this->notebook.setCompactMode(true);
    Q_EMIT this->status()->leftClicked();
    ASSERT_TRUE(this->notebook.areCompactTabsRevealed());

    // Native backends may first expose the press through the top-level
    // QWindow. It must not be mistaken for an outside click before Qt
    // dispatches the corresponding child-widget event.
    const auto global = secondTab->mapToGlobal(secondTab->rect().center());
    const auto windowLocal = this->host.windowHandle()->mapFromGlobal(global);
    QMouseEvent topLevelPress(QEvent::MouseButtonPress, QPointF(windowLocal),
                              QPointF(global), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
    QApplication::sendEvent(this->host.windowHandle(), &topLevelPress);
    ASSERT_TRUE(this->notebook.areCompactTabsRevealed());

    click(secondTab);

    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    EXPECT_TRUE(this->notebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_TRUE(secondInput->hasFocus());
}

TEST_F(CompactTabControlsTest,
       TabAndAddClicksStayRevealedAtEveryLocationWithExistingEmptyTab)
{
    auto *firstPage = new QWidget();
    auto *emptyPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *emptyTab = this->notebook.addPage(emptyPage, QStringLiteral("Empty"));
    auto *addButton = this->notebook.addTabButton();
    ASSERT_NE(addButton, nullptr);
    EXPECT_EQ(addButton->accessibleName(), QStringLiteral("Add tab"));

    int addCount = 0;
    QWidget *addedPage = nullptr;
    QObject::connect(addButton, &Button::leftClicked, &this->notebook, [&] {
        ++addCount;
        addedPage = new QWidget();
        this->notebook.addPage(addedPage, QStringLiteral("Added"), true);
    });

    constexpr std::array locations{
        NotebookTabLocation::Top,
        NotebookTabLocation::Bottom,
        NotebookTabLocation::Left,
        NotebookTabLocation::Right,
    };
    for (const auto location : locations)
    {
        this->notebook.setTabLocation(location);
        this->notebook.select(firstPage);
        this->notebook.setCompactMode(true);
        this->notebook.setCompactTabsRevealed(true);
        QCoreApplication::processEvents();

        click(emptyTab);
        EXPECT_EQ(this->notebook.getSelectedPage(), emptyPage);
        EXPECT_TRUE(this->notebook.areCompactTabsRevealed())
            << "location=" << static_cast<int>(location);

        const auto addGlobal =
            addButton->mapToGlobal(addButton->rect().center());
        const auto addWindowLocal =
            this->host.windowHandle()->mapFromGlobal(addGlobal);
        QMouseEvent topLevelAddPress(
            QEvent::MouseButtonPress, QPointF(addWindowLocal),
            QPointF(addGlobal), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(this->host.windowHandle(), &topLevelAddPress);
        ASSERT_TRUE(this->notebook.areCompactTabsRevealed())
            << "location=" << static_cast<int>(location);

        const auto previousCount = addCount;
        click(addButton);
        ASSERT_EQ(addCount, previousCount + 1)
            << "location=" << static_cast<int>(location);
        ASSERT_NE(addedPage, nullptr);
        EXPECT_EQ(this->notebook.getSelectedPage(), addedPage);
        EXPECT_TRUE(this->notebook.areCompactTabsRevealed())
            << "location=" << static_cast<int>(location);
        EXPECT_FALSE(addButton->isHidden());

        this->notebook.removePage(addedPage);
        addedPage = nullptr;
    }
}

TEST_F(CompactTabControlsTest, CloseButtonKeepsRevealAfterClosingTab)
{
    this->mockApplication.settings.showTabCloseButton.setValue(true);
    this->notebook.setAllowUserTabManagement(true);
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    this->notebook.addPage(secondPage, QStringLiteral("Second"));
    this->notebook.setCompactMode(true);
    this->notebook.setCompactTabsRevealed(true);
    QCoreApplication::processEvents();

    const auto closeLocal =
        QPointF(firstTab->width() - 8, firstTab->rect().center().y());
    const auto closeGlobal =
        QPointF(firstTab->mapToGlobal(closeLocal.toPoint()));
    QMouseEvent closePress(QEvent::MouseButtonPress, closeLocal, closeGlobal,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(firstTab, &closePress);
    EXPECT_TRUE(this->notebook.areCompactTabsRevealed());

    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto *yes = dialog->button(QMessageBox::Yes);
        ASSERT_NE(yes, nullptr);
        yes->click();
    });
    QMouseEvent closeRelease(QEvent::MouseButtonRelease, closeLocal,
                             closeGlobal, Qt::LeftButton, Qt::NoButton,
                             Qt::NoModifier);
    QApplication::sendEvent(firstTab, &closeRelease);
    QCoreApplication::processEvents();

    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    EXPECT_EQ(this->notebook.getVisibleTabCount(), 1);
    EXPECT_TRUE(this->notebook.areCompactTabsRevealed());
}

TEST_F(CompactTabControlsTest,
       RevealIsTemporaryAcrossModeAndUserVisibilityTransitions)
{
    auto *firstTab =
        this->notebook.addPage(new QWidget(), QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(new QWidget(), QStringLiteral("Second"));
    this->notebook.setUserTabsVisible(false);
    this->notebook.setCompactMode(true);

    Q_EMIT this->status()->leftClicked();
    ASSERT_TRUE(this->notebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_FALSE(this->notebook.userTabsVisible());

    this->notebook.setCompactMode(false);

    EXPECT_FALSE(this->notebook.areCompactTabsRevealed());
    EXPECT_FALSE(this->notebook.isCompactMode());
    EXPECT_TRUE(firstTab->isHidden());
    EXPECT_TRUE(secondTab->isHidden());
    EXPECT_FALSE(this->notebook.userTabsVisible());

    this->notebook.setUserTabsVisible(true);
    this->notebook.setCompactMode(true);
    Q_EMIT this->status()->leftClicked();
    ASSERT_TRUE(this->notebook.areCompactTabsRevealed());
    this->notebook.setCompactMode(false);
    EXPECT_FALSE(this->notebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
}

TEST_F(CompactTabControlsTest,
       ExternalCompactControlLayoutUsesTheSameRevealAndDismissState)
{
    this->notebook.hide();
    Notebook externalControlsNotebook(&this->host);
    externalControlsNotebook.setGeometry(this->host.rect());
    externalControlsNotebook.show();
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstTab = externalControlsNotebook.addPage(
        firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        externalControlsNotebook.addPage(secondPage, QStringLiteral("Second"));
    externalControlsNotebook.setCompactMode(true);
    ASSERT_TRUE(firstTab->isHidden());
    ASSERT_TRUE(secondTab->isHidden());

    externalControlsNotebook.setCompactTabsRevealed(true);

    EXPECT_TRUE(externalControlsNotebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_LT(firstPage->height(), externalControlsNotebook.height());

    click(secondTab);

    EXPECT_EQ(externalControlsNotebook.getSelectedPage(), secondPage);
    EXPECT_TRUE(externalControlsNotebook.areCompactTabsRevealed());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_LT(secondPage->height(), externalControlsNotebook.height());
}

TEST_F(CompactTabControlsTest, ExternalControlsReclaimNotebookChrome)
{
    this->notebook.hide();
    Notebook externalControlsNotebook(&this->host);
    externalControlsNotebook.setGeometry(this->host.rect());
    externalControlsNotebook.show();

    auto *page = new QWidget();
    externalControlsNotebook.addPage(page, QStringLiteral("First"), true);
    externalControlsNotebook.setCompactMode(true);
    QCoreApplication::processEvents();

    EXPECT_EQ(page->geometry(), externalControlsNotebook.rect());
    EXPECT_EQ(externalControlsNotebook.findChild<QWidget *>(
                  QStringLiteral("compactTabPrevious")),
              nullptr);
}

TEST_F(CompactTabControlsTest, StatusSynchronizesWithNotebookMutationsAndTitle)
{
    auto statusChangeCount = 0;
    const auto statusConnection =
        QObject::connect(&this->notebook, &Notebook::compactTabStatusChanged,
                         [&statusChangeCount] {
                             ++statusChangeCount;
                         });
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));
    this->notebook.setCompactMode(true);

    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 2 tabs"));
    EXPECT_EQ(this->notebook.getCompactSelectedTitle(),
              QStringLiteral("First"));

    this->notebook.select(secondPage);
    EXPECT_EQ(this->status()->text(), QStringLiteral("2 / 2 tabs"));
    EXPECT_EQ(this->notebook.getCompactSelectedTitle(),
              QStringLiteral("Second"));

    secondTab->setCustomTitle(QStringLiteral("Renamed"));
    EXPECT_EQ(this->notebook.getCompactSelectedTitle(),
              QStringLiteral("Renamed"));
    EXPECT_TRUE(this->status()->toolTip().contains(QStringLiteral("Renamed")));
    EXPECT_TRUE(
        this->status()->accessibleName().contains(QStringLiteral("Renamed")));

    this->notebook.rearrangePage(secondPage, 0);
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 2 tabs"));

    this->notebook.removePage(firstPage);
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 1 tabs"));
    EXPECT_FALSE(this->previous()->enabled());
    EXPECT_FALSE(this->next()->enabled());
    EXPECT_GT(statusChangeCount, 0);

    const auto stableCount = statusChangeCount;
    this->notebook.refresh();
    EXPECT_EQ(statusChangeCount, stableCount);
    QObject::disconnect(statusConnection);
}

TEST_F(CompactTabControlsTest, ControlsUseFilteredNotebookTraversalAndWrap)
{
    auto *firstPage = new QWidget();
    auto *filteredPage = new QWidget();
    auto *thirdPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *filteredTab =
        this->notebook.addPage(filteredPage, QStringLiteral("Filtered"));
    auto *thirdTab = this->notebook.addPage(thirdPage, QStringLiteral("Third"));
    firstTab->setLive(true);
    thirdTab->setLive(true);

    this->notebook.setVisibilityFilter([](const NotebookTab *tab) {
        return tab->isLive();
    });
    EXPECT_TRUE(filteredTab->isHidden());

    this->notebook.setCompactMode(true);
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 2 tabs"));

    filteredTab->setLive(true);
    this->notebook.refresh();
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 3 tabs"));

    filteredTab->setLive(false);
    this->notebook.refresh();
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 2 tabs"));

    this->notebook.select(nullptr);
    EXPECT_EQ(this->status()->text(), QStringLiteral("0 / 2 tabs"));
    EXPECT_TRUE(this->previous()->accessibleName().contains(
        QStringLiteral("No selected tab")));
    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);

    this->notebook.select(nullptr);
    Q_EMIT this->previous()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), thirdPage);
    EXPECT_EQ(this->status()->text(), QStringLiteral("2 / 2 tabs"));

    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);

    Q_EMIT this->previous()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), thirdPage);
}

TEST_F(CompactTabControlsTest, NavigationRestoresPageInputFocus)
{
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstInput = new QLineEdit(firstPage);
    auto *secondInput = new QLineEdit(secondPage);
    firstInput->setGeometry(0, 0, 200, 30);
    secondInput->setGeometry(0, 0, 200, 30);

    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));
    firstTab->setLive(true);
    secondTab->setLive(true);
    this->notebook.select(secondPage);
    secondInput->setFocus();
    QCoreApplication::processEvents();

    // Leaving without focusing the destination preserves the second page's
    // focused child for the regular Notebook selection API to restore.
    this->notebook.select(firstPage, false);
    firstInput->setFocus();
    this->notebook.setCompactMode(true);
    QCoreApplication::processEvents();
    ASSERT_TRUE(firstInput->hasFocus());
    ASSERT_EQ(this->next()->focusPolicy(), Qt::NoFocus);

    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    EXPECT_TRUE(secondInput->hasFocus());
    EXPECT_FALSE(this->next()->hasFocus());

    this->notebook.select(firstPage, false);
    firstInput->setFocus();
    this->notebook.refresh();
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    EXPECT_TRUE(secondInput->hasFocus());
    EXPECT_FALSE(this->nextLive()->hasFocus());
}

TEST_F(CompactTabControlsTest, AccessibleGeometrySurvivesLocationsAndScale)
{
    auto *firstTab = this->notebook.addPage(
        new QWidget(), QStringLiteral("Selected title"), true);
    auto *secondTab =
        this->notebook.addPage(new QWidget(), QStringLiteral("Second"));
    firstTab->setLive(true);
    secondTab->setLive(true);
    this->notebook.setCompactMode(true);

    const auto singleDigitWidth = this->status()->width();
    for (int i = 2; i < 12; ++i)
    {
        this->notebook.addPage(new QWidget(), QString::number(i));
    }
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 12 tabs"));
    EXPECT_GT(this->status()->width(), singleDigitWidth);

    ASSERT_TRUE(
        this->status()->toolTip().contains(QStringLiteral("Selected title")));
    ASSERT_TRUE(this->status()->accessibleDescription().contains(
        QStringLiteral("Selected title")));
    ASSERT_TRUE(this->previous()->accessibleName().contains(
        QStringLiteral("Selected title")));
    ASSERT_TRUE(this->next()->accessibleName().contains(
        QStringLiteral("Selected title")));

    constexpr std::array locations{
        NotebookTabLocation::Top,
        NotebookTabLocation::Bottom,
        NotebookTabLocation::Left,
        NotebookTabLocation::Right,
    };
    constexpr std::array scales{1.0F, 2.0F};

    for (const auto scale : scales)
    {
        this->mockApplication.settings.uiScale.setValue(scale);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        ASSERT_FLOAT_EQ(this->host.scale(), scale);
        for (const auto location : locations)
        {
            this->notebook.setTabLocation(location);
            this->notebook.refresh();
            QCoreApplication::processEvents();

            const std::array buttons{
                this->previousLive(), this->previous(), this->status(),
                this->next(), this->nextLive()};
            for (const auto *button : buttons)
            {
                ASSERT_FALSE(button->isHidden());
                EXPECT_GT(button->width(), 0);
                EXPECT_GT(button->height(), 0);
                EXPECT_TRUE(this->notebook.rect().contains(button->geometry()))
                    << "location=" << static_cast<int>(location)
                    << " scale=" << scale;
            }
            for (size_t index = 1; index < buttons.size(); ++index)
            {
                EXPECT_FALSE(buttons[index - 1]->geometry().intersects(
                    buttons[index]->geometry()));
            }
            ASSERT_NE(this->profile(), nullptr);
            EXPECT_EQ(this->profile()->geometry().right() + 1,
                      this->previousLive()->geometry().left());
        }
    }

    for (const auto &themeName :
         {QStringLiteral("Dark"), QStringLiteral("Light")})
    {
        this->mockApplication.theme.themeName.setValue(themeName);
        this->mockApplication.theme.update();
        QCoreApplication::processEvents();

        const auto foreground =
            this->host.palette().color(QPalette::WindowText);
        const auto background = this->host.palette().color(QPalette::Window);
        EXPECT_EQ(foreground, this->mockApplication.theme.window.text);
        if (!this->host.hasCustomWindowFrame())
        {
            EXPECT_EQ(background,
                      this->mockApplication.theme.window.background);
        }
        EXPECT_NE(foreground, background);
        EXPECT_EQ(this->status()->palette().color(QPalette::WindowText),
                  foreground);
        const auto liveControlColor =
            this->mockApplication.theme.isLightTheme()
                ? this->mockApplication.theme.tabs.liveIndicator.darker(135)
                : this->mockApplication.theme.tabs.liveIndicator;
        EXPECT_EQ(
            this->previousLive()->palette().color(QPalette::WindowText),
            liveControlColor);
        EXPECT_EQ(this->nextLive()->palette().color(QPalette::WindowText),
                  liveControlColor);
        EXPECT_NE(this->previousLive()->palette().color(QPalette::WindowText),
                  background);
        EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 12 tabs"));
        EXPECT_TRUE(this->status()->accessibleName().contains(
            QStringLiteral("Selected title")));
    }
}

TEST_F(ResponsiveTabsRegressionTest,
       TransitionMutationAndFilterMatrixPreservesState)
{
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *thirdPage = new QWidget();
    auto *firstInput = new QLineEdit(firstPage);
    firstInput->setGeometry(0, 0, 200, 30);

    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));
    auto *thirdTab =
        this->notebook.addPage(thirdPage, QStringLiteral("Third"));
    firstTab->setLive(true);
    secondTab->setLive(true);
    thirdTab->setLive(true);
    this->notebook.setVisibilityFilter([](const NotebookTab *tab) {
        return tab->isLive();
    });
    this->notebook.refresh();

    firstInput->setFocus();
    QCoreApplication::processEvents();
    ASSERT_TRUE(firstInput->hasFocus());

    this->notebook.setCompactMode(true);
    secondTab->setCustomTitle(QStringLiteral("Second renamed"));
    this->notebook.rearrangePage(thirdPage, 0);
    thirdTab->setLive(false);

    auto *fourthPage = new QWidget();
    auto *fourthTab =
        this->notebook.addPage(fourthPage, QStringLiteral("Fourth"));
    fourthTab->setLive(true);
    this->notebook.refresh();
    QCoreApplication::processEvents();

    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);
    EXPECT_TRUE(firstInput->hasFocus());
    EXPECT_EQ(this->status()->text(), QStringLiteral("1 / 3 tabs"));

    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    EXPECT_EQ(this->status()->text(), QStringLiteral("2 / 3 tabs"));
    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), fourthPage);
    Q_EMIT this->next()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);
    EXPECT_TRUE(firstInput->hasFocus());

    this->notebook.setCompactMode(false);
    QCoreApplication::processEvents();

    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);
    EXPECT_TRUE(firstInput->hasFocus());
    EXPECT_TRUE(this->notebook.userTabsVisible());
    EXPECT_FALSE(firstTab->isHidden());
    EXPECT_FALSE(secondTab->isHidden());
    EXPECT_TRUE(thirdTab->isHidden());
    EXPECT_FALSE(fourthTab->isHidden());
}

TEST_F(ResponsiveTabsRegressionTest,
       HideAllPreferenceSurvivesMutationsAndTransitions)
{
    auto *firstPage = new QWidget();
    auto *secondPage = new QWidget();
    auto *firstTab =
        this->notebook.addPage(firstPage, QStringLiteral("First"), true);
    auto *secondTab =
        this->notebook.addPage(secondPage, QStringLiteral("Second"));
    firstTab->setLive(true);
    secondTab->setLive(true);

    this->notebook.setUserTabsVisible(false);
    ASSERT_FALSE(this->notebook.userTabsVisible());
    this->notebook.setCompactMode(true);
    this->notebook.refresh();
    ASSERT_TRUE(this->nextLive()->enabled());
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), secondPage);
    Q_EMIT this->nextLive()->leftClicked();
    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);

    secondTab->setCustomTitle(QStringLiteral("Renamed"));
    this->notebook.rearrangePage(secondPage, 0);
    auto *thirdTab =
        this->notebook.addPage(new QWidget(), QStringLiteral("Third"));
    this->notebook.refresh();
    this->notebook.setCompactMode(false);

    EXPECT_EQ(this->notebook.getSelectedPage(), firstPage);
    EXPECT_FALSE(this->notebook.userTabsVisible());
    EXPECT_TRUE(firstTab->isHidden());
    EXPECT_TRUE(secondTab->isHidden());
    EXPECT_TRUE(thirdTab->isHidden());
}

}  // namespace chatterino
