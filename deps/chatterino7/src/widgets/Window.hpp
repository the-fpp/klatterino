// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"
#include "widgets/helper/CompactTabControls.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QMetaObject>
#include <QPointer>

#include <vector>

class QScreen;
class QWindow;

namespace chatterino {

class PixmapButton;
class LabelButton;
class Theme;
class UpdateDialog;
class SplitNotebook;
class Channel;
#ifdef CHATTERINO_HAVE_PLUGINS
class PluginControlTabHost;
#endif

/**
 * @exposeenum c2.WindowType
 */
enum class WindowType { Main, Popup, Attached };

class Window : public BaseWindow
{
    Q_OBJECT
    Q_PROPERTY(bool compactMode READ isCompactMode NOTIFY compactModeChanged)

public:
    explicit Window(WindowType type, QWidget *parent);

    WindowType getType();
    SplitNotebook &getNotebook();
    bool isCompactMode() const;
#ifdef CHATTERINO_HAVE_PLUGINS
    PluginControlTabHost *getPluginControlTabHost() const;
#endif

    pajlada::Signals::NoArgSignal closed;

Q_SIGNALS:
    /// Emitted only when responsive geometry changes the compact-mode state.
    /// Manual tab visibility and live-tab filters are separate Notebook state.
    void compactModeChanged(bool compactMode);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;
    void themeChangedEvent() override;

private:
    void addCustomTitlebarButtons();
    void addDebugStuff(
        std::map<QString, std::function<QString(std::vector<QString>)>>
            &actions);
    void addShortcuts() override;
    void addLayout();
    void onAccountSelected();
    void addMenuBar();
    void connectCompactModeWindow();
    void connectCompactModeScreen(QScreen *screen);
    QScreen *containingScreen() const;
    void updateCompactMode();
    void updateCompactTabControls();

    WindowType type_;

    SplitNotebook *notebook_;
#ifdef CHATTERINO_HAVE_PLUGINS
    PluginControlTabHost *pluginControlTabHost_ = nullptr;
#endif
    LabelButton *userLabel_ = nullptr;
    CompactTabControlButtons compactTabControls_;
    std::shared_ptr<UpdateDialog> updateDialogHandle_;

    bool compactMode_ = false;
    QPointer<QWindow> compactModeWindowHandle_;
    QPointer<QScreen> compactModeScreen_;
    QMetaObject::Connection compactModeWindowScreenConnection_;
    std::vector<QMetaObject::Connection> compactModeScreenConnections_;

    pajlada::Signals::SignalHolder signalHolder_;

    // this is only used on Windows and only on the main window, for the one used otherwise, see SplitNotebook in Notebook.hpp
    PixmapButton *streamerModeTitlebarIcon_ = nullptr;
    void updateStreamerModeIcon();

    friend class Notebook;
};

}  // namespace chatterino
