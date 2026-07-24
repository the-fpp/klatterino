// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/PluginControlTab.hpp"
#    include "widgets/BaseWidget.hpp"

#    include <QPointer>

#    include <vector>

class QEvent;
class QFrame;
class QHBoxLayout;
class QMenu;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class Window;

/// Host-owned rendering surface for bounded plugin control tabs. It consumes
/// value snapshots only and never stores a Plugin, Lua state, or callback.
class PluginControlTabHost final : public BaseWidget
{
    Q_OBJECT

public:
    explicit PluginControlTabHost(Window *window);
    ~PluginControlTabHost() override;

    QString openPluginID() const;
    std::size_t visibleTabCount() const;
    std::size_t overflowTabCount() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    struct TabEntry {
        PluginControlTabSnapshot snapshot;
        QToolButton *button = nullptr;
        bool overflowed = false;
    };

    void rebuild();
    void rebuildTabs();
    void rebuildPanel();
    void updateOverflow();
    void applyTheme();
    void openPanel(const QString &pluginID);
    void closePanel(bool restoreFocus);
    void showInvocationError(const QString &error);
    bool ownsObject(const QObject *object) const;

    Window *window_;
    QWidget *tabRow_;
    QHBoxLayout *tabLayout_;
    QToolButton *overflowButton_;
    QMenu *overflowMenu_;
    QFrame *panel_ = nullptr;
    QVBoxLayout *layout_;
    std::vector<TabEntry> tabs_;
    QString openPluginID_;
    QPointer<QWidget> restoreFocus_;
    bool applicationFilterInstalled_ = false;
};

}  // namespace chatterino

#endif
