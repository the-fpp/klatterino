// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SelectChannelDialog.hpp"

#include "Application.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/helper/MicroNotebook.hpp"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {

using namespace chatterino;

constexpr int MULTI_CHANNEL_ACTIVE_ROLE = Qt::UserRole + 1;

bool isActiveMultiChannelItem(const QListWidgetItem *item)
{
    return item && item->data(MULTI_CHANNEL_ACTIVE_ROLE).toBool();
}

QListWidgetItem *activeMultiChannelItem(const QListWidget *view)
{
    if (!view)
    {
        return nullptr;
    }
    for (int row = 0; row < view->count(); ++row)
    {
        auto *item = view->item(row);
        if (isActiveMultiChannelItem(item))
        {
            return item;
        }
    }
    return nullptr;
}

void setActiveMultiChannelItem(QListWidget *view, QListWidgetItem *active)
{
    if (!view || (active && active->listWidget() != view))
    {
        return;
    }

    const QSignalBlocker blocker(view);
    for (int row = 0; row < view->count(); ++row)
    {
        auto *item = view->item(row);
        const bool selected = item == active;
        item->setData(MULTI_CHANNEL_ACTIVE_ROLE, selected);
        auto font = item->font();
        font.setBold(selected);
        item->setFont(font);
        item->setToolTip(selected ? QStringLiteral("Active context")
                                  : QString{});
    }
}

class AddToMultiChannel : public BasePopup
{
    Q_OBJECT

public:
    AddToMultiChannel(QWidget *parent = nullptr)
        : BasePopup(
              {
                  BaseWindow::EnableCustomFrame,
                  BaseWindow::DisableLayoutSave,
                  BaseWindow::BoundsCheckOnShow,
              },
              parent)
        , platform(new QComboBox)
        , name(new QLineEdit)
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setWindowTitle("Add Channel");

        auto *layout = new QVBoxLayout(this->getLayoutContainer());

        this->platform->addItem(
            "Twitch", QVariant::fromValue(MultiChannel::Platform::Twitch));
        this->platform->addItem(
            "Kick", QVariant::fromValue(MultiChannel::Platform::Kick));
        this->platform->addItem(
            "Rumble", QVariant::fromValue(MultiChannel::Platform::Rumble));
        layout->addWidget(this->platform);

        this->name->setPlaceholderText("Name");
        layout->addWidget(this->name);

        this->error = new QLabel;
        this->error->setWordWrap(true);
        layout->addWidget(this->error);

        this->buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        QObject::connect(this->buttons, &QDialogButtonBox::accepted, this,
                         &AddToMultiChannel::accept);
        QObject::connect(this->buttons, &QDialogButtonBox::rejected, this,
                         &AddToMultiChannel::close);
        layout->addStretch();
        layout->addWidget(this->buttons);

        this->addShortcuts();
        this->name->setFocus();
    }

    void addShortcuts() override
    {
        HotkeyController::HotkeyMap actions{
            {"accept",
             [this](const std::vector<QString> &) -> QString {
                 this->accept();
                 return {};
             }},
            {"reject",
             [this](const std::vector<QString> &) -> QString {
                 this->close();
                 return {};
             }},
        };

        this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
            HotkeyCategory::PopupWindow, actions, this);
    }

Q_SIGNALS:
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    void specAdded(chatterino::MultiChannel::Spec spec);

private:
    void accept()
    {
        if (this->request.active())
        {
            return;
        }
        auto nameText = this->name->text();
        auto platform =
            this->platform->currentData().value<MultiChannel::Platform>();
        if (nameText.isEmpty())
        {
            return;
        }

        if (platform != MultiChannel::Platform::Rumble)
        {
            this->specAdded(MultiChannel::Spec{
                .platform = platform,
                .name = nameText,
            });
            this->close();
            return;
        }

        auto *controller = getApp()->getRumble();
        if (!controller)
        {
            this->error->setText(QStringLiteral("Rumble is unavailable."));
            return;
        }
        this->error->clear();
        this->name->setEnabled(false);
        this->platform->setEnabled(false);
        this->buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        const auto generation = ++this->generation;
        QPointer<AddToMultiChannel> guarded(this);
        this->request = controller->resolve(
            nameText,
            [guarded, generation](
                RumbleApplicationController::ResolveResult result) mutable {
                if (!guarded || guarded->generation != generation)
                {
                    return;
                }
                guarded->name->setEnabled(true);
                guarded->platform->setEnabled(true);
                guarded->buttons->button(QDialogButtonBox::Ok)
                    ->setEnabled(true);
                if (!result)
                {
                    guarded->error->setText(result.error().userMessage);
                    return;
                }
                const auto &identity = result->layoutIdentity();
                if (!identity || identity->locator.isEmpty())
                {
                    guarded->error->setText(QStringLiteral(
                        "Rumble couldn't use that link. Try a channel or "
                        "video URL."));
                    return;
                }
                guarded->specAdded(MultiChannel::Spec{
                    .platform = MultiChannel::Platform::Rumble,
                    .name = identity->locator,
                    .layoutIdentity = *identity,
                });
                guarded->close();
            });
        if (!this->request.active() && !this->name->isEnabled())
        {
            this->name->setEnabled(true);
            this->platform->setEnabled(true);
            this->buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            this->error->setText(
                QStringLiteral("Couldn't connect to Rumble. Try again."));
        }
    }

    void closeEvent(QCloseEvent *event) override
    {
        ++this->generation;
        this->request.cancel();
        BasePopup::closeEvent(event);
    }

    QComboBox *platform = nullptr;
    QLineEdit *name = nullptr;
    QLabel *error = nullptr;
    QDialogButtonBox *buttons = nullptr;
    std::uint64_t generation = 0;
    RumbleApplicationController::Request request;
};

QListWidgetItem *makeMultiChannelItem(const MultiChannel::Spec &spec)
{
    QString name;
    switch (spec.platform)
    {
        case MultiChannel::Platform::Twitch:
            name += u"[T] ";
            break;
        case MultiChannel::Platform::Kick:
            name += u"[K] ";
            break;
        case MultiChannel::Platform::Rumble:
            name += u"[R] ";
            break;
    }
    name += spec.name;
    auto *item = new QListWidgetItem(name);
    item->setData(Qt::UserRole, QVariant::fromValue(spec));
    item->setData(MULTI_CHANNEL_ACTIVE_ROLE, false);
    return item;
}

QListWidgetItem *makeMultiChannelItem(const MultiChannel::ChildChannel &chan)
{
    return makeMultiChannelItem(chan.spec());
}

}  // namespace

namespace chatterino {

SelectChannelDialog::SelectChannelDialog(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , selectedChannel_(Channel::getEmpty())
{
    using AutoCheckedRadioButton = detail::AutoCheckedRadioButton;

    this->setWindowTitle("Select a channel to join");

    this->tabFilter_.dialog = this;

    auto &ui = this->ui_;
    auto *rootLayout = new QVBoxLayout(this->getLayoutContainer());
    rootLayout->setContentsMargins({});
    ui.notebook = new MicroNotebook(this->getLayoutContainer());
    rootLayout->addWidget(ui.notebook, 1);

    ui.twitchPage = new QWidget;
    auto *layout = new QVBoxLayout(ui.twitchPage);

    // Channel
    ui.channel = new AutoCheckedRadioButton("Channel");
    layout->addWidget(ui.channel);

    ui.channelLabel = new QLabel("Join a Twitch channel by its channel name");
    ui.channelLabel->setVisible(false);
    layout->addWidget(ui.channelLabel);

    ui.channelName = new QLineEdit();
    ui.channelName->setVisible(false);
    layout->addWidget(ui.channelName);

    QObject::connect(ui.channel, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.channelName->setVisible(enabled);
                         ui.channelLabel->setVisible(enabled);

                         if (enabled)
                         {
                             ui.channelName->setFocus();
                             ui.channelName->selectAll();
                         }
                     });

    ui.channel->installEventFilter(&this->tabFilter_);
    ui.channelName->installEventFilter(&this->tabFilter_);

    // Whispers
    ui.whispers = new AutoCheckedRadioButton("Whispers");
    layout->addWidget(ui.whispers);

    ui.whispersLabel = new QLabel(
        "Shows the whispers that you receive while Chatterino is running");
    ui.whispersLabel->setVisible(false);
    ui.whispersLabel->setWordWrap(true);
    layout->addWidget(ui.whispersLabel);

    QObject::connect(ui.whispers, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.whispersLabel->setVisible(enabled);
                     });

    ui.whispers->installEventFilter(&this->tabFilter_);

    // Mentions
    ui.mentions = new AutoCheckedRadioButton("Mentions");
    layout->addWidget(ui.mentions);

    ui.mentionsLabel = new QLabel(
        "Shows all the messages that highlight you from any channel");
    ui.mentionsLabel->setVisible(false);
    ui.mentionsLabel->setWordWrap(true);
    layout->addWidget(ui.mentionsLabel);

    QObject::connect(ui.mentions, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.mentionsLabel->setVisible(enabled);
                     });

    ui.mentions->installEventFilter(&this->tabFilter_);

    // Watching
    ui.watching = new AutoCheckedRadioButton("Watching");
    layout->addWidget(ui.watching);

    ui.watchingLabel = new QLabel("Requires the Chatterino browser extension");
    ui.watchingLabel->setVisible(false);
    layout->addWidget(ui.watchingLabel);

    QObject::connect(ui.watching, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.watchingLabel->setVisible(enabled);
                     });

    ui.watching->installEventFilter(&this->tabFilter_);

    // Live
    ui.live = new AutoCheckedRadioButton("Live");
    layout->addWidget(ui.live);

    ui.liveLabel = new QLabel("Shows when channels go live");
    ui.liveLabel->setVisible(false);
    layout->addWidget(ui.liveLabel);

    QObject::connect(ui.live, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.liveLabel->setVisible(enabled);
                     });

    ui.live->installEventFilter(&this->tabFilter_);

    // Automod
    ui.automod = new AutoCheckedRadioButton("AutoMod");
    layout->addWidget(ui.automod);

    ui.automodLabel = new QLabel("Shows when AutoMod catches a message in "
                                 "any channel you moderate.");
    ui.automodLabel->setVisible(false);
    ui.automodLabel->setWordWrap(true);
    layout->addWidget(ui.automodLabel);

    QObject::connect(ui.automod, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.automodLabel->setVisible(enabled);
                     });

    ui.automod->installEventFilter(&this->tabFilter_);

    layout->addStretch(1);

    ui.notebook->addPage(ui.twitchPage, "Twitch");

    // Kick
    {
        ui.kickPage = new QWidget;
        auto *layout = new QVBoxLayout(ui.kickPage);

        auto *kickLabel = new QLabel(
            "Join a Kick channel by its name.<br>This is <b>very "
            "experimental</b> and Chatterino7 specific. Only basic features "
            "are supported. Please report bugs <a "
            "href=\"https://github.com/SevenTV/chatterino7/issues\">here</a>.");
        kickLabel->setOpenExternalLinks(true);
        kickLabel->setWordWrap(true);
        layout->addWidget(kickLabel);

        ui.kickName = new QLineEdit();
        ui.kickName->setPlaceholderText("Username");
        layout->addWidget(ui.kickName);

        layout->addStretch(1);

        ui.notebook->addPage(ui.kickPage, "Kick");
    }
    // Rumble
    {
        ui.rumblePage = new QWidget;
        auto *layout = new QVBoxLayout(ui.rumblePage);

        auto *description = new QLabel(
            "Join a public Rumble channel or video. Use its channel or video "
            "URL so Chatterino can find it again when it is offline.");
        description->setWordWrap(true);
        layout->addWidget(description);

        ui.rumbleLocator = new QLineEdit;
        ui.rumbleLocator->setPlaceholderText(
            "https://rumble.com/c/channel or video URL");
        layout->addWidget(ui.rumbleLocator);

        ui.rumbleError = new QLabel;
        ui.rumbleError->setWordWrap(true);
        layout->addWidget(ui.rumbleError);
        layout->addStretch(1);

        ui.notebook->addPage(ui.rumblePage, "Rumble");
    }
    // Multi
    {
        ui.multiPage = new QWidget;
        ui.multiView = new QListWidget;
        ui.multiIndicatorMode = new QComboBox;
        auto *layout = new QVBoxLayout(ui.multiPage);
        {
            auto *descriptionLabel = new QLabel(
                "Show multiple channels in one split. From the input box, you "
                "can select an active/context channel to send messages in. "
                "Report issues <a "
                "href=\"https://github.com/SevenTV/chatterino7/issues\">here</"
                "a>.");
            descriptionLabel->setWordWrap(true);
            descriptionLabel->setOpenExternalLinks(true);
            layout->addWidget(descriptionLabel);

            auto *header = new QWidget;
            auto *add = new QPushButton("Add");
            auto *remove = new QPushButton("Remove");
            auto *setActive = new QPushButton("Set Active");
            auto *headerLayout = new QHBoxLayout(header);
            headerLayout->addWidget(add, 1);
            headerLayout->addWidget(remove, 1);
            headerLayout->addWidget(setActive, 1);
            layout->addWidget(header);

            QObject::connect(add, &QPushButton::clicked, this, [this] {
                auto *diag = new AddToMultiChannel(this);
                QObject::connect(diag, &AddToMultiChannel::specAdded, this,
                                 [this](const MultiChannel::Spec &spec) {
                                     auto *item = makeMultiChannelItem(spec);
                                     this->ui_.multiView->addItem(item);
                                     if (!activeMultiChannelItem(
                                             this->ui_.multiView))
                                     {
                                         setActiveMultiChannelItem(
                                             this->ui_.multiView, item);
                                     }
                                 });
                diag->show();
            });
            QObject::connect(remove, &QPushButton::clicked, this, [this] {
                auto *view = this->ui_.multiView;
                const int removedRow = view->currentRow();
                if (removedRow < 0)
                {
                    return;
                }
                auto *removed = view->takeItem(removedRow);
                const bool removedActive = isActiveMultiChannelItem(removed);
                delete removed;
                if (removedActive && view->count() > 0)
                {
                    setActiveMultiChannelItem(
                        view, view->item(std::min(removedRow,
                                                 view->count() - 1)));
                }
            });
            QObject::connect(setActive, &QPushButton::clicked, this, [this] {
                if (auto *item = this->ui_.multiView->currentItem())
                {
                    setActiveMultiChannelItem(this->ui_.multiView, item);
                }
            });
        }

        ui.multiView->setAutoScroll(true);
        ui.multiView->setSelectionMode(QListWidget::SingleSelection);
        ui.multiView->setSelectionBehavior(QListWidget::SelectRows);
        ui.multiView->setDragDropMode(QListWidget::InternalMove);
        ui.multiView->setFrameStyle(QFrame::NoFrame);
        ui.multiView->setSizeAdjustPolicy(QListView::AdjustToContents);
        layout->addWidget(ui.multiView, 1);

        layout->addWidget(new QLabel("Channel indicator:"));
        {
            using Mode = MultiChannelIndicatorMode;

            auto v = [](Mode mode) {
                return QVariant::fromValue(mode);
            };
            ui.multiIndicatorMode->addItem("None", v(Mode::None));
            ui.multiIndicatorMode->addItem("Platform badge if unselected",
                                           v(Mode::PlatformBadgeIfUnselected));
            ui.multiIndicatorMode->addItem("Platform badge",
                                           v(Mode::PlatformBadgeAlways));
            ui.multiIndicatorMode->addItem("Channel name",
                                           v(Mode::ChannelName));
            ui.multiIndicatorMode->setCurrentIndex(1);
        }
        layout->addWidget(ui.multiIndicatorMode);

        ui.notebook->addPage(ui.multiPage, "Multi");
    }

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ui.okButton = buttonBox->button(QDialogButtonBox::Ok);
    buttonBox->setContentsMargins({10, 10, 10, 10});
    rootLayout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        this->ok();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, [this] {
        this->close();
    });

    this->addShortcuts();

    this->themeChangedEvent();
}

void SelectChannelDialog::ok()
{
    if (this->closing_ || this->rumbleRequest_.active())
    {
        return;
    }
    if (this->ui_.notebook->isSelected(this->ui_.rumblePage))
    {
        auto *controller = getApp()->getRumble();
        if (!controller)
        {
            this->ui_.rumbleError->setText(
                QStringLiteral("Rumble is unavailable."));
            return;
        }
        this->ui_.rumbleError->clear();
        this->ui_.rumbleLocator->setEnabled(false);
        this->ui_.notebook->setEnabled(false);
        this->ui_.okButton->setEnabled(false);
        const auto generation = ++this->rumbleGeneration_;
        QPointer<SelectChannelDialog> guarded(this);
        this->rumbleRequest_ = controller->resolve(
            this->ui_.rumbleLocator->text(),
            [guarded, generation](
                RumbleApplicationController::ResolveResult result) mutable {
                if (!guarded || guarded->closing_ ||
                    guarded->rumbleGeneration_ != generation)
                {
                    return;
                }
                guarded->ui_.rumbleLocator->setEnabled(true);
                guarded->ui_.notebook->setEnabled(true);
                guarded->ui_.okButton->setEnabled(true);
                if (!guarded->ui_.notebook->isSelected(guarded->ui_.rumblePage))
                {
                    return;
                }
                if (!result)
                {
                    guarded->ui_.rumbleError->setText(
                        result.error().userMessage);
                    return;
                }
                guarded->selectedChannel_ = std::move(*result);
                guarded->hasSelectedChannel_ = true;
                guarded->close();
            });
        if (!this->rumbleRequest_.active() &&
            !this->ui_.rumbleLocator->isEnabled())
        {
            this->ui_.rumbleLocator->setEnabled(true);
            this->ui_.notebook->setEnabled(true);
            this->ui_.okButton->setEnabled(true);
            this->ui_.rumbleError->setText(
                QStringLiteral("Couldn't connect to Rumble. Try again."));
        }
        return;
    }
    // accept and close
    this->hasSelectedChannel_ = true;
    this->close();
}

void SelectChannelDialog::setSelectedChannel(
    std::optional<IndirectChannel> channel_)
{
    if (!channel_.has_value())
    {
        this->ui_.channel->setChecked(true);

        this->hasSelectedChannel_ = false;
        return;
    }

    const auto &indirectChannel = channel_.value();
    const auto &channel = indirectChannel.get();

    assert(channel);

    this->selectedChannel_ = indirectChannel;

    switch (indirectChannel.getType())
    {
        case Channel::Type::Twitch: {
            this->ui_.channelName->setText(channel->getName());
            this->ui_.channel->setChecked(true);
        }
        break;
        case Channel::Type::TwitchWatching: {
            this->ui_.watching->setFocus();
        }
        break;
        case Channel::Type::TwitchMentions: {
            this->ui_.mentions->setFocus();
        }
        break;
        case Channel::Type::TwitchWhispers: {
            this->ui_.whispers->setFocus();
        }
        break;
        case Channel::Type::TwitchLive: {
            this->ui_.live->setFocus();
        }
        break;
        case Channel::Type::TwitchAutomod: {
            this->ui_.automod->setFocus();
        }
        break;
        case Channel::Type::Kick: {
            this->ui_.kickName->setText(channel->getName());
            this->ui_.kickName->selectAll();
            this->ui_.notebook->select(this->ui_.kickPage);
        }
        break;
        case Channel::Type::Rumble: {
            if (const auto &identity = indirectChannel.layoutIdentity())
            {
                this->ui_.rumbleLocator->setText(identity->locator);
            }
            this->ui_.rumbleLocator->selectAll();
            this->ui_.notebook->select(this->ui_.rumblePage);
        }
        break;
        case Channel::Type::Multi: {
            const auto *mc = dynamic_cast<const MultiChannel *>(channel.get());
            if (mc)
            {
                for (const auto &child : mc->channels())
                {
                    this->ui_.multiView->addItem(makeMultiChannelItem(child));
                }
                if (this->ui_.multiView->count() > 0)
                {
                    const auto selected = std::min<size_t>(
                        mc->activeChannelIndex(),
                        static_cast<size_t>(this->ui_.multiView->count() - 1));
                    auto *item = this->ui_.multiView->item(
                        static_cast<int>(selected));
                    setActiveMultiChannelItem(this->ui_.multiView, item);
                    this->ui_.multiView->setCurrentItem(item);
                }
                int indicatorIdx = this->ui_.multiIndicatorMode->findData(
                    QVariant::fromValue(mc->indicatorMode()));
                if (indicatorIdx >= 0)
                {
                    this->ui_.multiIndicatorMode->setCurrentIndex(indicatorIdx);
                }
            }
            this->ui_.notebook->select(this->ui_.multiPage);
        }
        break;
        default: {
            this->ui_.channel->setChecked(true);
        }
    }

    this->hasSelectedChannel_ = false;
}

IndirectChannel SelectChannelDialog::getSelectedChannel() const
{
    if (!this->hasSelectedChannel_)
    {
        return this->selectedChannel_;
    }

    if (this->ui_.notebook->isSelected(this->ui_.rumblePage))
    {
        return this->selectedChannel_;
    }

    if (this->ui_.notebook->isSelected(this->ui_.kickPage))
    {
        return getApp()->getKickChatServer()->getOrCreate(
            this->ui_.kickName->text().trimmed());
    }

    if (this->ui_.notebook->isSelected(this->ui_.multiPage))
    {
        QVarLengthArray<MultiChannel::Spec, 4> specs;
        std::optional<int> selectedRow;
        std::optional<size_t> selectedIndex;
        std::vector<std::pair<int, size_t>> validRows;
        for (int i = 0; i < this->ui_.multiView->count(); i++)
        {
            auto *item = this->ui_.multiView->item(i);
            if (!item)
            {
                continue;
            }
            if (isActiveMultiChannelItem(item))
            {
                selectedRow = i;
            }
            QVariant data = item->data(Qt::UserRole);
            auto *spec = get_if<MultiChannel::Spec>(&data);
            if (spec)
            {
                const auto index = static_cast<size_t>(specs.size());
                specs.emplace_back(*spec);
                validRows.emplace_back(i, index);
                if (isActiveMultiChannelItem(item))
                {
                    selectedIndex = index;
                }
            }
        }
        if (!selectedIndex && selectedRow && !validRows.empty())
        {
            const auto next = std::ranges::find_if(
                validRows, [selectedRow](const auto &entry) {
                    return entry.first >= *selectedRow;
                });
            selectedIndex = next != validRows.end() ? next->second
                                                    : validRows.back().second;
        }
        auto ptr = std::make_shared<MultiChannel>(
            specs, this->ui_.multiIndicatorMode->currentData()
                       .value<MultiChannelIndicatorMode>());
        ptr->setActiveChannelIndex(selectedIndex.value_or(0));
        return {std::move(ptr)};
    }

    if (this->ui_.channel->isChecked())
    {
        return getApp()->getTwitch()->getOrAddChannel(
            this->ui_.channelName->text().trimmed());
    }

    if (this->ui_.watching->isChecked())
    {
        return getApp()->getTwitch()->getWatchingChannel();
    }

    if (this->ui_.mentions->isChecked())
    {
        return getApp()->getTwitch()->getMentionsChannel();
    }

    if (this->ui_.whispers->isChecked())
    {
        return getApp()->getTwitch()->getWhispersChannel();
    }

    if (this->ui_.live->isChecked())
    {
        return getApp()->getTwitch()->getLiveChannel();
    }

    if (this->ui_.automod->isChecked())
    {
        return getApp()->getTwitch()->getAutomodChannel();
    }

    return this->selectedChannel_;
}

bool SelectChannelDialog::hasSeletedChannel() const
{
    return this->hasSelectedChannel_;
}

bool SelectChannelDialog::EventFilter::eventFilter(QObject *watched,
                                                   QEvent *event)
{
    auto *widget = dynamic_cast<QWidget *>(watched);
    assert(widget);

    auto &ui = this->dialog->ui_;

    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
        assert(keyEvent);

        if ((keyEvent->key() == Qt::Key_Tab ||
             keyEvent->key() == Qt::Key_Down) &&
            keyEvent->modifiers() == Qt::NoModifier)
        {
            // Tab has been pressed, focus next entry in list

            if (widget == ui.channelName)
            {
                // Special case for when current selection is the "Channel" entry's edit box since the Edit box actually has the focus
                ui.whispers->setFocus();
                return true;
            }

            if (widget == ui.automod)
            {
                // Special case for when current selection is "AutoMod" (the last entry in the list), next wrap is Channel, but we need to select its edit box
                ui.channel->setFocus();
                return true;
            }

            auto *nextInFocusChain = widget->nextInFocusChain();
            if (nextInFocusChain->focusPolicy() == Qt::FocusPolicy::NoFocus)
            {
                // Make sure we're not selecting one of the labels
                nextInFocusChain = nextInFocusChain->nextInFocusChain();
            }
            nextInFocusChain->setFocus();
            return true;
        }

        if (((keyEvent->key() == Qt::Key_Tab ||
              keyEvent->key() == Qt::Key_Backtab) &&
             keyEvent->modifiers() == Qt::ShiftModifier) ||
            ((keyEvent->key() == Qt::Key_Up) &&
             keyEvent->modifiers() == Qt::NoModifier))
        {
            // Shift+Tab has been pressed, focus previous entry in list

            if (widget == ui.channelName)
            {
                // Special case for when current selection is the "Channel" entry's edit box since the Edit box actually has the focus
                ui.automod->setFocus();
                return true;
            }

            if (widget == ui.whispers)
            {
                ui.channel->setFocus();
                return true;
            }

            auto *previousInFocusChain = widget->previousInFocusChain();
            if (previousInFocusChain->focusPolicy() == Qt::FocusPolicy::NoFocus)
            {
                // Make sure we're not selecting one of the labels
                previousInFocusChain =
                    previousInFocusChain->previousInFocusChain();
            }
            previousInFocusChain->setFocus();
            return true;
        }

        if (keyEvent == QKeySequence::DeleteStartOfWord &&
            ui.channelName->selectionLength() > 0)
        {
            ui.channelName->backspace();
            return true;
        }

        return false;
    }

    return false;
}

void SelectChannelDialog::closeEvent(QCloseEvent * /*event*/)
{
    this->closing_ = true;
    ++this->rumbleGeneration_;
    this->rumbleRequest_.cancel();
    this->closed.invoke();
}

void SelectChannelDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    this->setPalette(getTheme()->palette);
}

void SelectChannelDialog::scaleChangedEvent(float newScale)
{
    BaseWindow::scaleChangedEvent(newScale);

    auto &ui = this->ui_;

    // NOTE: Normally the font is automatically inherited from its parent, but since we override
    // the style sheet to respect light/dark theme, we have to manually update the font here
    auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());

    ui.channelName->setFont(uiFont);
    ui.rumbleLocator->setFont(uiFont);
}

void SelectChannelDialog::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"accept",
         [this](const std::vector<QString> &) -> QString {
             this->ok();
             return "";
         }},
        {"reject",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        // these make no sense, so they aren't implemented
        {"scrollPage", nullptr},
        {"search", nullptr},
        {"delete", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

}  // namespace chatterino

#include "SelectChannelDialog.moc"
