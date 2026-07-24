// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "widgets/PluginControlTabHost.hpp"

#    include "Application.hpp"
#    include "common/QLogging.hpp"
#    include "controllers/plugins/PluginController.hpp"
#    include "singletons/Theme.hpp"
#    include "widgets/Window.hpp"

#    include <QApplication>
#    include <QAction>
#    include <QCheckBox>
#    include <QComboBox>
#    include <QCursor>
#    include <QEvent>
#    include <QFontMetrics>
#    include <QFormLayout>
#    include <QFrame>
#    include <QHBoxLayout>
#    include <QIcon>
#    include <QKeyEvent>
#    include <QLabel>
#    include <QMenu>
#    include <QMouseEvent>
#    include <QProgressBar>
#    include <QPushButton>
#    include <QResizeEvent>
#    include <QSignalBlocker>
#    include <QStyle>
#    include <QTimer>
#    include <QToolButton>
#    include <QToolTip>
#    include <QVBoxLayout>

#    include <algorithm>
#    include <cmath>

namespace chatterino {
namespace {

QString formattedValue(const std::optional<double> &value,
                       const QString &units)
{
    if (!value.has_value())
    {
        return {};
    }
    auto text = QString::number(*value, 'g', 7);
    if (!units.isEmpty())
    {
        text += QStringLiteral(" ") + units;
    }
    return text;
}

QString severityText(PluginControlTabSeverity severity)
{
    switch (severity)
    {
        case PluginControlTabSeverity::Neutral:
            return {};
        case PluginControlTabSeverity::Active:
            return QStringLiteral("Active");
        case PluginControlTabSeverity::Success:
            return QStringLiteral("OK");
        case PluginControlTabSeverity::Warning:
            return QStringLiteral("Warning");
        case PluginControlTabSeverity::Error:
            return QStringLiteral("Error");
        case PluginControlTabSeverity::Unknown:
            return QStringLiteral("Unknown");
    }
    return {};
}

QIcon iconFor(QWidget *widget, const QString &name)
{
    auto pixmap = QStyle::SP_DriveNetIcon;
    if (name == QStringLiteral("play"))
    {
        pixmap = QStyle::SP_MediaPlay;
    }
    else if (name == QStringLiteral("pause"))
    {
        pixmap = QStyle::SP_MediaPause;
    }
    else if (name == QStringLiteral("stop"))
    {
        pixmap = QStyle::SP_MediaStop;
    }
    else if (name == QStringLiteral("refresh"))
    {
        pixmap = QStyle::SP_BrowserReload;
    }
    else if (name == QStringLiteral("settings"))
    {
        pixmap = QStyle::SP_FileDialogDetailedView;
    }
    else if (name == QStringLiteral("info"))
    {
        pixmap = QStyle::SP_MessageBoxInformation;
    }
    else if (name == QStringLiteral("warning"))
    {
        pixmap = QStyle::SP_MessageBoxWarning;
    }
    else if (name == QStringLiteral("error"))
    {
        pixmap = QStyle::SP_MessageBoxCritical;
    }
    else if (name == QStringLiteral("check"))
    {
        pixmap = QStyle::SP_DialogApplyButton;
    }
    if (name.isEmpty())
    {
        return {};
    }
    return widget->style()->standardIcon(pixmap);
}

QString collapsedText(const PluginControlTabSnapshot &snapshot)
{
    // Lead with the plugin-authored purpose so it survives bounded elision;
    // keep the owning plugin alongside it so the contribution cannot look
    // native or impersonate another application surface.
    auto text = QStringLiteral("%1 · %2")
                    .arg(snapshot.title, snapshot.pluginName);
    if (!snapshot.summary.primaryText.isEmpty())
    {
        text += QStringLiteral(" — ") + snapshot.summary.primaryText;
    }
    const auto value = formattedValue(snapshot.summary.numericValue,
                                      snapshot.summary.units);
    if (!value.isEmpty())
    {
        text += QStringLiteral(" · ") + value;
    }
    return text;
}

QString accessibleSummary(const PluginControlTabSnapshot &snapshot)
{
    QStringList parts;
    parts << QStringLiteral("%1 plugin controls: %2")
                 .arg(snapshot.pluginName, snapshot.title);
    if (!snapshot.summary.primaryText.isEmpty())
    {
        parts << snapshot.summary.primaryText;
    }
    if (!snapshot.summary.secondaryText.isEmpty())
    {
        parts << snapshot.summary.secondaryText;
    }
    const auto value = formattedValue(snapshot.summary.numericValue,
                                      snapshot.summary.units);
    if (!value.isEmpty())
    {
        parts << value;
    }
    const auto severity = severityText(snapshot.summary.severity);
    if (!severity.isEmpty())
    {
        parts << severity;
    }
    if (snapshot.summary.stale)
    {
        parts << QStringLiteral("Stale");
    }
    return parts.join(QStringLiteral(". "));
}

QString tooltipText(const PluginControlTabSnapshot &snapshot)
{
    QStringList parts;
    parts << QStringLiteral("Controls contributed by %1")
                 .arg(snapshot.pluginName);
    if (!snapshot.tooltip.isEmpty())
    {
        parts << snapshot.tooltip;
    }
    if (!snapshot.summary.secondaryText.isEmpty())
    {
        parts << snapshot.summary.secondaryText;
    }
    return parts.join(QStringLiteral("\n"));
}

}  // namespace

PluginControlTabHost::PluginControlTabHost(Window *window)
    : BaseWidget(window)
    , window_(window)
    , tabRow_(new QWidget(this))
    , tabLayout_(new QHBoxLayout(this->tabRow_))
    , overflowButton_(new QToolButton(this->tabRow_))
    , overflowMenu_(new QMenu(this->overflowButton_))
    , layout_(new QVBoxLayout(this))
{
    this->setObjectName(QStringLiteral("plugin-control-tab-host"));
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    this->setVisible(false);

    this->tabRow_->setObjectName(QStringLiteral("plugin-control-tab-row"));
    this->tabRow_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    this->tabLayout_->setContentsMargins(4, 2, 4, 0);
    this->tabLayout_->setSpacing(3);
    this->tabLayout_->addStretch(1);

    this->overflowButton_->setObjectName(
        QStringLiteral("plugin-control-tab-overflow"));
    this->overflowButton_->setText(QStringLiteral("More…"));
    this->overflowButton_->setAccessibleName(
        QStringLiteral("More plugin control tabs"));
    this->overflowButton_->setPopupMode(QToolButton::InstantPopup);
    this->overflowButton_->setMenu(this->overflowMenu_);
    this->overflowButton_->setVisible(false);
    this->tabLayout_->addWidget(this->overflowButton_);

    this->layout_->setContentsMargins(0, 0, 0, 0);
    this->layout_->setSpacing(0);
    this->layout_->addWidget(this->tabRow_);

    this->signalHolder_.managedConnect(
        getApp()->getPlugins()->onControlTabsUpdated, [this] {
            this->rebuild();
        });
    this->rebuild();
}

PluginControlTabHost::~PluginControlTabHost()
{
    if (this->applicationFilterInstalled_ && qApp != nullptr)
    {
        qApp->removeEventFilter(this);
    }
}

QString PluginControlTabHost::openPluginID() const
{
    return this->openPluginID_;
}

std::size_t PluginControlTabHost::visibleTabCount() const
{
    return std::ranges::count_if(this->tabs_, [](const auto &entry) {
        return !entry.overflowed;
    });
}

std::size_t PluginControlTabHost::overflowTabCount() const
{
    return std::ranges::count_if(this->tabs_, [](const auto &entry) {
        return entry.overflowed;
    });
}

void PluginControlTabHost::rebuild()
{
    const auto oldOpen = this->openPluginID_;
    auto *focus = QApplication::focusWidget();
    const bool focusWasInPanel =
        this->panel_ != nullptr && focus != nullptr &&
        (focus == this->panel_ || this->panel_->isAncestorOf(focus));
    const auto focusedName = focusWasInPanel ? focus->objectName() : QString{};

    this->tabs_.clear();
    for (auto snapshot : getApp()->getPlugins()->controlTabs())
    {
        if (snapshot.visible)
        {
            this->tabs_.push_back({std::move(snapshot), nullptr, false});
        }
    }
    this->rebuildTabs();

    const auto selected = std::ranges::find(
        this->tabs_, oldOpen,
        [](const auto &entry) { return entry.snapshot.pluginId; });
    if (selected == this->tabs_.end() ||
        !selected->snapshot.summary.openEnabled)
    {
        this->closePanel(true);
    }
    else
    {
        this->openPluginID_ = oldOpen;
        this->rebuildPanel();
        if (focusWasInPanel && !focusedName.isEmpty())
        {
            if (auto *replacement =
                    this->panel_->findChild<QWidget *>(focusedName))
            {
                replacement->setFocus(Qt::OtherFocusReason);
            }
        }
    }

    this->updateOverflow();
    this->setVisible(!this->tabs_.empty());
    this->applyTheme();
    QTimer::singleShot(0, this, [this] {
        this->updateOverflow();
    });
}

void PluginControlTabHost::rebuildTabs()
{
    while (this->tabLayout_->count() > 2)
    {
        auto *item = this->tabLayout_->takeAt(0);
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    for (std::size_t index = 0; index < this->tabs_.size(); ++index)
    {
        auto &entry = this->tabs_[index];
        auto *button = new QToolButton(this->tabRow_);
        entry.button = button;
        button->setObjectName(QStringLiteral("plugin-control-tab-%1")
                                  .arg(entry.snapshot.pluginId));
        button->setProperty("pluginControlTabOwner",
                            entry.snapshot.pluginId);
        button->setCheckable(true);
        button->setChecked(entry.snapshot.pluginId == this->openPluginID_);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setText(collapsedText(entry.snapshot));
        button->setIcon(iconFor(button, entry.snapshot.icon));
        button->setToolTip(tooltipText(entry.snapshot));
        button->setAccessibleName(
            entry.snapshot.accessibleLabel.isEmpty()
                ? accessibleSummary(entry.snapshot)
                : QStringLiteral("%1 plugin controls: %2")
                      .arg(entry.snapshot.pluginName,
                           entry.snapshot.accessibleLabel));
        button->setAccessibleDescription(tooltipText(entry.snapshot));
        button->setEnabled(entry.snapshot.summary.openEnabled);
        button->setFocusPolicy(Qt::StrongFocus);
        // Keep newly discovered contributions out of the layout's minimum
        // width until the bounded overflow pass decides which ones fit. This
        // prevents asynchronous plugin loading from growing a top-level
        // window to the combined width of every tab.
        button->setVisible(false);
        QObject::connect(button, &QToolButton::clicked, this,
                         [this, id = entry.snapshot.pluginId] {
                             if (this->openPluginID_ == id)
                             {
                                 this->closePanel(true);
                             }
                             else
                             {
                                 this->openPanel(id);
                             }
                         });
        this->tabLayout_->insertWidget(static_cast<int>(index), button);
    }
}

void PluginControlTabHost::openPanel(const QString &pluginID)
{
    const auto selected = std::ranges::find(
        this->tabs_, pluginID,
        [](const auto &entry) { return entry.snapshot.pluginId; });
    if (selected == this->tabs_.end() ||
        !selected->snapshot.summary.openEnabled)
    {
        return;
    }
    if (this->openPluginID_.isEmpty())
    {
        this->restoreFocus_ = QApplication::focusWidget();
    }
    this->openPluginID_ = pluginID;
    this->rebuildPanel();
    if (this->panel_ != nullptr)
    {
        const auto controls = this->panel_->findChildren<QWidget *>();
        const auto firstFocusable = std::ranges::find_if(
            controls, [](const auto *control) {
                return control->objectName().startsWith(
                           QStringLiteral("plugin-control-")) &&
                       (control->focusPolicy() & Qt::TabFocus) != 0 &&
                       control->isEnabled();
            });
        if (firstFocusable != controls.end())
        {
            (*firstFocusable)->setFocus(Qt::TabFocusReason);
        }
    }
    for (auto &entry : this->tabs_)
    {
        entry.button->setChecked(entry.snapshot.pluginId == pluginID);
    }
    if (!this->applicationFilterInstalled_)
    {
        qApp->installEventFilter(this);
        this->applicationFilterInstalled_ = true;
    }
}

void PluginControlTabHost::closePanel(bool restoreFocus)
{
    if (this->panel_ == nullptr && this->openPluginID_.isEmpty())
    {
        return;
    }
    this->openPluginID_.clear();
    if (this->panel_ != nullptr)
    {
        this->layout_->removeWidget(this->panel_);
        this->panel_->hide();
        this->panel_->deleteLater();
        this->panel_ = nullptr;
    }
    for (auto &entry : this->tabs_)
    {
        if (entry.button != nullptr)
        {
            entry.button->setChecked(false);
        }
    }
    if (this->applicationFilterInstalled_)
    {
        qApp->removeEventFilter(this);
        this->applicationFilterInstalled_ = false;
    }
    if (restoreFocus && this->restoreFocus_ != nullptr &&
        this->restoreFocus_->window() == this->window_)
    {
        this->restoreFocus_->setFocus(Qt::OtherFocusReason);
    }
    this->restoreFocus_.clear();
}

void PluginControlTabHost::rebuildPanel()
{
    if (this->panel_ != nullptr)
    {
        this->layout_->removeWidget(this->panel_);
        this->panel_->hide();
        this->panel_->deleteLater();
        this->panel_ = nullptr;
    }
    const auto selected = std::ranges::find(
        this->tabs_, this->openPluginID_,
        [](const auto &entry) { return entry.snapshot.pluginId; });
    if (selected == this->tabs_.end())
    {
        return;
    }
    const auto &snapshot = selected->snapshot;
    auto *panel = new QFrame(this);
    this->panel_ = panel;
    panel->setObjectName(QStringLiteral("plugin-control-tab-panel"));
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setAccessibleName(
        QStringLiteral("%1 plugin control panel").arg(snapshot.pluginName));
    auto *outer = new QVBoxLayout(panel);
    outer->setContentsMargins(int(8 * this->scale()),
                             int(6 * this->scale()),
                             int(8 * this->scale()),
                             int(8 * this->scale()));
    outer->setSpacing(int(5 * this->scale()));

    auto *header = new QLabel(
        QStringLiteral("Controls from %1 — %2")
            .arg(snapshot.pluginName, snapshot.title),
        panel);
    QFont headerFont = header->font();
    headerFont.setBold(true);
    header->setFont(headerFont);
    header->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                    Qt::TextSelectableByMouse);
    header->setAccessibleName(
        QStringLiteral("Controls contributed by %1").arg(snapshot.pluginName));
    outer->addWidget(header);

    if (!snapshot.summary.secondaryText.isEmpty())
    {
        auto *summary = new QLabel(snapshot.summary.secondaryText, panel);
        summary->setWordWrap(true);
        summary->setAccessibleName(QStringLiteral("Plugin status details"));
        outer->addWidget(summary);
    }

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    outer->addLayout(form);

    for (const auto &control : snapshot.controls)
    {
        if (!control.visible)
        {
            continue;
        }
        const auto objectName =
            QStringLiteral("plugin-control-%1-%2")
                .arg(snapshot.pluginId, control.id);
        auto *label = new QLabel(control.label, panel);
        label->setWordWrap(true);
        label->setToolTip(control.description);

        QWidget *fieldWidget = nullptr;
        switch (control.type)
        {
            case PluginControlType::Status: {
                auto *status = new QWidget(panel);
                auto *statusLayout = new QVBoxLayout(status);
                statusLayout->setContentsMargins(0, 0, 0, 0);
                statusLayout->setSpacing(2);
                QStringList pieces;
                if (!control.text.isEmpty())
                {
                    pieces << control.text;
                }
                const auto value =
                    formattedValue(control.numericValue, control.units);
                if (!value.isEmpty())
                {
                    pieces << value;
                }
                if (control.stale)
                {
                    pieces << QStringLiteral("Stale");
                }
                auto *text = new QLabel(pieces.join(QStringLiteral(" · ")),
                                        status);
                text->setWordWrap(true);
                statusLayout->addWidget(text);
                if (control.progress.has_value())
                {
                    auto *progress = new QProgressBar(status);
                    progress->setRange(0, 1000);
                    progress->setValue(
                        qRound(std::clamp(*control.progress, 0.0, 1.0) *
                               1000.0));
                    progress->setTextVisible(true);
                    progress->setAccessibleName(
                        QStringLiteral("%1 progress").arg(control.label));
                    statusLayout->addWidget(progress);
                }
                fieldWidget = status;
                break;
            }
            case PluginControlType::Action: {
                auto *button = new QPushButton(control.label, panel);
                button->setIcon(iconFor(button, control.icon));
                button->setEnabled(control.enabled && !control.pending);
                QObject::connect(
                    button, &QPushButton::clicked, this,
                    [this, pluginID = snapshot.pluginId,
                     controlID = control.id] {
                        auto result = getApp()->getPlugins()->invokeControlTab(
                            pluginID, controlID, std::monostate{});
                        if (!result)
                        {
                            this->showInvocationError(result.error());
                        }
                    });
                fieldWidget = button;
                label->setVisible(false);
                break;
            }
            case PluginControlType::Toggle: {
                auto *toggle = new QCheckBox(control.label, panel);
                toggle->setChecked(control.toggleValue);
                toggle->setEnabled(control.enabled && !control.pending);
                QObject::connect(
                    toggle, &QCheckBox::clicked, this,
                    [this, toggle, pluginID = snapshot.pluginId,
                     controlID = control.id,
                     authoritative = control.toggleValue](bool proposed) {
                        auto result = getApp()->getPlugins()->invokeControlTab(
                            pluginID, controlID, proposed);
                        {
                            const QSignalBlocker blocker(toggle);
                            toggle->setChecked(authoritative);
                        }
                        if (!result)
                        {
                            this->showInvocationError(result.error());
                        }
                    });
                fieldWidget = toggle;
                label->setVisible(false);
                break;
            }
            case PluginControlType::Choice: {
                auto *choice = new QComboBox(panel);
                for (const auto &option : control.choices)
                {
                    choice->addItem(option.label, option.value);
                }
                const auto selectedIndex =
                    choice->findData(control.choiceValue);
                choice->setCurrentIndex(selectedIndex);
                choice->setEnabled(control.enabled && !control.pending);
                QObject::connect(
                    choice, &QComboBox::activated, this,
                    [this, choice, pluginID = snapshot.pluginId,
                     controlID = control.id,
                     authoritative = control.choiceValue](int index) {
                        const auto proposed = choice->itemData(index).toString();
                        auto result = getApp()->getPlugins()->invokeControlTab(
                            pluginID, controlID, proposed);
                        {
                            const QSignalBlocker blocker(choice);
                            choice->setCurrentIndex(
                                choice->findData(authoritative));
                        }
                        if (!result)
                        {
                            this->showInvocationError(result.error());
                        }
                    });
                fieldWidget = choice;
                break;
            }
        }
        if (fieldWidget == nullptr)
        {
            continue;
        }
        fieldWidget->setObjectName(objectName);
        fieldWidget->setToolTip(control.description);
        fieldWidget->setAccessibleName(
            QStringLiteral("%1: %2").arg(snapshot.pluginName, control.label));
        auto description = control.description;
        if (control.pending)
        {
            if (!description.isEmpty())
            {
                description += QStringLiteral(". ");
            }
            description += QStringLiteral("Waiting for the plugin to confirm");
        }
        fieldWidget->setAccessibleDescription(description);
        form->addRow(label, fieldWidget);
    }

    this->layout_->addWidget(panel);
    this->applyTheme();
}

void PluginControlTabHost::updateOverflow()
{
    if (this->tabs_.empty())
    {
        this->overflowButton_->setVisible(false);
        return;
    }
    const int available = std::max(0, this->tabRow_->width() -
                                          this->tabLayout_->contentsMargins().left() -
                                          this->tabLayout_->contentsMargins().right());
    const int spacing = this->tabLayout_->spacing();
    const int overflowWidth =
        std::max(int(64 * this->scale()),
                 this->overflowButton_->sizeHint().width());
    int required = 0;
    for (const auto &entry : this->tabs_)
    {
        required += std::min(int(240 * this->scale()),
                             entry.button->sizeHint().width()) +
                    spacing;
    }
    const bool needsOverflow = required > available;
    int remaining = available - (needsOverflow ? overflowWidth + spacing : 0);
    std::size_t shown = 0;
    for (auto &entry : this->tabs_)
    {
        const int desired = std::clamp(entry.button->sizeHint().width(),
                                       int(78 * this->scale()),
                                       int(240 * this->scale()));
        const bool show = !needsOverflow ||
                          (remaining >= desired || shown == 0);
        entry.overflowed = !show;
        entry.button->setVisible(show);
        entry.button->setMaximumWidth(desired);
        if (show)
        {
            remaining -= desired + spacing;
            ++shown;
        }
    }

    this->overflowMenu_->clear();
    for (const auto &entry : this->tabs_)
    {
        if (!entry.overflowed)
        {
            continue;
        }
        auto *action = this->overflowMenu_->addAction(
            iconFor(this->overflowButton_, entry.snapshot.icon),
            collapsedText(entry.snapshot));
        action->setToolTip(tooltipText(entry.snapshot));
        action->setEnabled(entry.snapshot.summary.openEnabled);
        QObject::connect(action, &QAction::triggered, this,
                         [this, id = entry.snapshot.pluginId] {
                             this->openPanel(id);
                         });
    }
    this->overflowButton_->setVisible(needsOverflow);
    this->overflowButton_->setAccessibleDescription(
        QStringLiteral("%1 additional plugin control tabs")
            .arg(this->overflowTabCount()));
}

void PluginControlTabHost::showInvocationError(const QString &error)
{
    qCWarning(chatterinoLua) << "Plugin control-tab interaction failed:"
                            << error;
    QToolTip::showText(QCursor::pos(),
                       QStringLiteral("Plugin control failed: %1").arg(error),
                       this);
}

bool PluginControlTabHost::ownsObject(const QObject *object) const
{
    for (auto *cursor = object; cursor != nullptr; cursor = cursor->parent())
    {
        if (cursor == this)
        {
            return true;
        }
    }
    return false;
}

bool PluginControlTabHost::eventFilter(QObject *watched, QEvent *event)
{
    if (this->panel_ == nullptr)
    {
        return BaseWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress)
    {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape)
        {
            this->closePanel(true);
            return true;
        }
    }
    else if (event->type() == QEvent::MouseButtonPress &&
             !this->ownsObject(watched))
    {
        this->closePanel(false);
    }
    return BaseWidget::eventFilter(watched, event);
}

void PluginControlTabHost::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateOverflow();
}

void PluginControlTabHost::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->applyTheme();
}

void PluginControlTabHost::scaleChangedEvent(float newScale)
{
    BaseWidget::scaleChangedEvent(newScale);
    this->tabLayout_->setContentsMargins(int(4 * newScale), int(2 * newScale),
                                         int(4 * newScale), 0);
    this->tabLayout_->setSpacing(int(3 * newScale));
    this->updateOverflow();
}

void PluginControlTabHost::applyTheme()
{
    if (this->theme == nullptr)
    {
        return;
    }
    const auto regular = this->theme->tabs.regular.backgrounds.regular.name(
        QColor::HexArgb);
    const auto hover =
        this->theme->tabs.regular.backgrounds.hover.name(QColor::HexArgb);
    const auto selected =
        this->theme->tabs.selected.backgrounds.regular.name(QColor::HexArgb);
    const auto text = this->theme->tabs.regular.text.name(QColor::HexArgb);
    const auto border = this->theme->tabs.dividerLine.name(QColor::HexArgb);
    const auto style = QStringLiteral(
                           "QToolButton { color: %1; background: %2; border: "
                           "1px solid %3; border-top-left-radius: 8px; "
                           "border-top-right-radius: 8px; padding: 4px 7px; } "
                           "QToolButton:hover { background: %4; } "
                           "QToolButton:checked { background: %5; "
                           "border-bottom: 2px solid %6; } "
                           "QToolButton:disabled { color: palette(mid); }")
                           .arg(text, regular, border, hover, selected,
                                this->theme->accent.name(QColor::HexArgb));
    this->tabRow_->setStyleSheet(style);
    if (this->panel_ != nullptr)
    {
        this->panel_->setPalette(this->theme->palette);
        this->panel_->setAutoFillBackground(true);
    }
}

}  // namespace chatterino
#endif
