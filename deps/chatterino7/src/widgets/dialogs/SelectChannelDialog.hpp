// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "providers/rumble/RumbleApplicationController.hpp"
#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signal.hpp>
#include <QComboBox>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>

#include <optional>

namespace chatterino::detail {

/// a radio button that checks itself when it receives focus
class AutoCheckedRadioButton : public QRadioButton
{
public:
    AutoCheckedRadioButton(const QString &label)
        : QRadioButton(label)
    {
    }

protected:
    void focusInEvent(QFocusEvent * /*event*/) override
    {
        this->setChecked(true);
    }
};

}  // namespace chatterino::detail

namespace chatterino {

class EditableModelView;
class IndirectChannel;
class MicroNotebook;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

class SelectChannelDialog final : public BaseWindow
{
public:
    SelectChannelDialog(QWidget *parent = nullptr);

    void setSelectedChannel(std::optional<IndirectChannel> channel_);
    IndirectChannel getSelectedChannel() const;
    bool hasSeletedChannel() const;

    pajlada::Signals::NoArgSignal closed;

protected:
    void closeEvent(QCloseEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    class EventFilter : public QObject
    {
    public:
        SelectChannelDialog *dialog;

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override;
    };

    struct {
        detail::AutoCheckedRadioButton *channel;
        QLabel *channelLabel;
        QLineEdit *channelName;

        detail::AutoCheckedRadioButton *whispers;
        QLabel *whispersLabel;

        detail::AutoCheckedRadioButton *mentions;
        QLabel *mentionsLabel;

        detail::AutoCheckedRadioButton *watching;
        QLabel *watchingLabel;

        detail::AutoCheckedRadioButton *live;
        QLabel *liveLabel;

        detail::AutoCheckedRadioButton *automod;
        QLabel *automodLabel;

        QLineEdit *kickName;

        QLineEdit *rumbleLocator;
        QLabel *rumbleError;

        QListWidget *multiView;
        QComboBox *multiIndicatorMode;

        MicroNotebook *notebook;
        QWidget *twitchPage;
        QWidget *kickPage;
        QWidget *rumblePage;
        QWidget *multiPage;
        QPushButton *okButton;
    } ui_{};

    EventFilter tabFilter_;

    IndirectChannel selectedChannel_;
    bool hasSelectedChannel_ = false;
    bool closing_ = false;
    std::uint64_t rumbleGeneration_ = 0;
    RumbleApplicationController::Request rumbleRequest_;

    void ok();
    friend class EventFilter;

    void addShortcuts() override;
};

}  // namespace chatterino
