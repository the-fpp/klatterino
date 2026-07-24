// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/layouts/MessageLayout.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace chatterino {

class MessageHistorySelectionController;

class MessageHistorySelectionOverlay final : public QWidget
{
public:
    MessageHistorySelectionOverlay(
        QWidget *parent, MessageHistorySelectionController &controller)
        : QWidget(parent)
        , controller_(controller)
    {
        this->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->setAttribute(Qt::WA_NoSystemBackground);
        this->hide();
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    MessageHistorySelectionController &controller_;
};

class MessageHistorySelectionController final : public QObject
{
public:
    explicit MessageHistorySelectionController(QTextEdit &owner)
        : owner_(&owner)
    {
        // ResizingTextEdit installs its own event filter in its constructor.
        // Install this one on the next event-loop turn so selection-mode keys
        // are seen before the normal shortcut routing.
        QTimer::singleShot(0, &owner, [this] {
            if (this->owner_ != nullptr)
            {
                this->owner_->installEventFilter(this);
            }
        });
    }

    std::optional<QRectF> selectionRect() const
    {
        if (!this->active_ || this->view_.isNull())
        {
            return std::nullopt;
        }

        const auto range = this->selectedRange();
        if (!range)
        {
            return std::nullopt;
        }

        auto &messages = this->view_->getMessagesSnapshot();
        if (messages.empty() || range->first >= messages.size() ||
            range->second >= messages.size())
        {
            return std::nullopt;
        }

        const qreal relative =
            this->view_->getScrollBar().getRelativeCurrentValue();
        const auto start = static_cast<size_t>(std::floor(relative));
        if (start >= messages.size())
        {
            return std::nullopt;
        }

        const auto heightAt = [&messages](size_t index) -> qreal {
            return std::max<qreal>(1, messages[index]->getHeight());
        };

        qreal y = -heightAt(start) * std::fmod(relative, 1.0);
        if (range->first >= start)
        {
            for (size_t i = start; i < range->first; ++i)
            {
                y += heightAt(i);
            }
        }
        else
        {
            for (size_t i = range->first; i < start; ++i)
            {
                y -= heightAt(i);
            }
        }

        qreal height = 0;
        for (size_t i = range->first; i <= range->second; ++i)
        {
            height += heightAt(i);
        }

        auto right = this->view_->width() - 4;
        if (auto *scrollbar = this->view_->scrollbar();
            scrollbar != nullptr && scrollbar->isVisible())
        {
            right -= scrollbar->width();
        }

        return QRectF(3, y + 1, std::max(1, right - 3),
                      std::max<qreal>(1, height - 2));
    }

    bool individualMode() const
    {
        return this->individualMode_;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->view_.data() &&
            (event->type() == QEvent::Resize ||
             event->type() == QEvent::Show))
        {
            this->resizeOverlay();
            return false;
        }

        if (watched != this->owner_)
        {
            return false;
        }

        if (event->type() == QEvent::ShortcutOverride)
        {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const auto modifiers =
                keyEvent->modifiers() & ~Qt::KeypadModifier;
            const auto key = keyEvent->key();
            const bool navigation =
                (modifiers == Qt::AltModifier ||
                 (this->active_ && modifiers == Qt::NoModifier)) &&
                (key == Qt::Key_J || key == Qt::Key_K);
            const bool activeAction =
                this->active_ &&
                ((key == Qt::Key_Escape && modifiers == Qt::NoModifier) ||
                 (key == Qt::Key_Backspace && modifiers == Qt::NoModifier) ||
                 ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
                  (modifiers == Qt::NoModifier ||
                   modifiers == Qt::ControlModifier)));

            if (navigation || activeAction)
            {
                event->accept();
                return true;
            }
            return false;
        }

        if (event->type() != QEvent::KeyPress)
        {
            return false;
        }

        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const auto key = keyEvent->key();
        const auto modifiers = keyEvent->modifiers() & ~Qt::KeypadModifier;

        const bool navigation =
            (modifiers == Qt::AltModifier ||
             (this->active_ && modifiers == Qt::NoModifier)) &&
            (key == Qt::Key_J || key == Qt::Key_K);
        if (navigation)
        {
            this->moveSelection(key == Qt::Key_K);
            keyEvent->accept();
            return true;
        }

        if (!this->active_)
        {
            return false;
        }

        if (key == Qt::Key_Escape && modifiers == Qt::NoModifier)
        {
            this->exitToInput();
            keyEvent->accept();
            return true;
        }

        if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
            modifiers == Qt::ControlModifier)
        {
            this->copySelectionToInput();
            keyEvent->accept();
            return true;
        }

        if (key == Qt::Key_Backspace && modifiers == Qt::NoModifier)
        {
            this->handleReply();
            keyEvent->accept();
            return true;
        }

        if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
            modifiers == Qt::NoModifier)
        {
            if (this->individualMode_)
            {
                this->individualMode_ = false;
                this->scrollSelectionToCenter();
                this->updateOverlay();
            }
            keyEvent->accept();
            return true;
        }

        return false;
    }

private:
    struct Block {
        size_t first;
        size_t last;
        QString text;
    };

    Split *findSplit() const
    {
        for (QWidget *widget = this->owner_; widget != nullptr;
             widget = widget->parentWidget())
        {
            if (auto *split = qobject_cast<Split *>(widget))
            {
                return split;
            }
        }
        return nullptr;
    }

    bool activate()
    {
        auto *split = this->findSplit();
        if (split == nullptr)
        {
            return false;
        }

        this->split_ = split;
        this->view_ = &split->getChannelView();
        this->view_->getMessagesSnapshot();
        auto &scrollbar = this->view_->getScrollBar();
        this->scrollingToBottomBeforeSelection_ =
            this->view_->getEnableScrollingToBottom();
        this->wasAtBottomBeforeSelection_ = scrollbar.isAtBottom();
        this->relativeScrollBeforeSelection_ =
            scrollbar.getDesiredValue() - scrollbar.getMinimum();
        this->view_->pause(PauseReason::Selection);
        this->view_->setEnableScrollingToBottom(false);
        this->buildBlocks();

        if (this->blocks_.empty())
        {
            this->view_->setEnableScrollingToBottom(
                this->scrollingToBottomBeforeSelection_);
            this->view_->unpause(PauseReason::Selection);
            this->view_->getMessagesSnapshot();
            this->view_.clear();
            this->split_.clear();
            return false;
        }

        if (this->overlay_.isNull())
        {
            this->overlay_ =
                new MessageHistorySelectionOverlay(this->view_, *this);
            this->view_->installEventFilter(this);
        }

        this->active_ = true;
        this->individualMode_ = false;
        this->blockIndex_ = this->blocks_.size() - 1;
        this->itemIndex_ = this->blocks_.back().last;
        this->resizeOverlay();
        this->overlay_->show();
        this->overlay_->raise();
        this->scrollSelectionToCenter();
        this->updateOverlay();
        return true;
    }

    void buildBlocks()
    {
        this->blocks_.clear();
        if (this->view_.isNull())
        {
            return;
        }

        auto &messages = this->view_->getMessagesSnapshot();
        bool previousWasSelectable = false;

        for (size_t i = 0; i < messages.size(); ++i)
        {
            const auto message = messages[i]->getMessagePtr();
            const bool selectable =
                message != nullptr &&
                !message->flags.hasAny(MessageFlag::System,
                                       MessageFlag::Disabled,
                                       MessageFlag::Timeout) &&
                !message->messageText.trimmed().isEmpty();

            if (!selectable)
            {
                previousWasSelectable = false;
                continue;
            }

            if (previousWasSelectable && !this->blocks_.empty() &&
                this->blocks_.back().last + 1 == i &&
                this->blocks_.back().text == message->messageText)
            {
                this->blocks_.back().last = i;
            }
            else
            {
                this->blocks_.push_back({i, i, message->messageText});
            }
            previousWasSelectable = true;
        }
    }

    void moveSelection(bool older)
    {
        if (!this->active_)
        {
            this->activate();
            return;
        }

        if (this->individualMode_)
        {
            const auto &block = this->blocks_[this->blockIndex_];
            if (older && this->itemIndex_ > block.first)
            {
                --this->itemIndex_;
            }
            else if (!older && this->itemIndex_ < block.last)
            {
                ++this->itemIndex_;
            }
        }
        else if (older && this->blockIndex_ > 0)
        {
            --this->blockIndex_;
        }
        else if (!older && this->blockIndex_ + 1 < this->blocks_.size())
        {
            ++this->blockIndex_;
        }

        if (!this->individualMode_)
        {
            this->itemIndex_ = this->blocks_[this->blockIndex_].last;
        }

        this->scrollSelectionToCenter();
        this->updateOverlay();
    }

    void handleReply()
    {
        if (!this->active_)
        {
            return;
        }

        const auto &block = this->blocks_[this->blockIndex_];
        if (!this->individualMode_ && block.first != block.last)
        {
            this->individualMode_ = true;
            this->itemIndex_ = block.last;
            this->scrollSelectionToCenter();
            this->updateOverlay();
            return;
        }

        const auto message = this->selectedMessage();
        if (message == nullptr || this->view_.isNull() || this->split_.isNull())
        {
            return;
        }

        auto channel = this->view_->inferChannel(*message);
        if (channel == nullptr)
        {
            return;
        }

        const auto replyStatus = message->isReplyable();

        // Rumble does not expose reply-thread sends yet, but binding a local
        // draft is still useful while logged out. SplitInput enforces provider
        // support only when the user attempts to send and preserves the draft.
        if (channel->isRumbleChannel())
        {
            switch (replyStatus)
            {
                case Message::ReplyStatus::Replyable:
                case Message::ReplyStatus::ReplyableWithThread:
                    this->split_->setInputReply(message, channel);
                    break;
                default:
                    this->view_->underlyingChannel()->addSystemMessage(
                        QStringLiteral(
                            "This Rumble message cannot be used as a reply. "
                            "Use Ctrl+Enter in the picker to copy a message "
                            "instead."));
                    break;
            }
            this->exitToInput();
            return;
        }

        switch (replyStatus)
        {
            case Message::ReplyStatus::Replyable:
            case Message::ReplyStatus::ReplyableWithThread:
                break;
            default:
                return;
        }

        if (!channel->isTwitchOrKickChannel())
        {
            return;
        }

        if (message->replyThread == nullptr)
        {
            if (auto *twitch = dynamic_cast<TwitchChannel *>(channel.get()))
            {
                twitch->getOrCreateThread(message);
            }
            else if (auto *kick = dynamic_cast<KickChannel *>(channel.get()))
            {
                kick->getOrCreateThread(message->id);
            }
            else
            {
                return;
            }
        }

        this->split_->setInputReply(message, channel);
        this->exitToInput();
    }

    void copySelectionToInput()
    {
        const auto message = this->selectedMessage();
        if (message == nullptr || this->split_.isNull())
        {
            return;
        }

        this->split_->getInput().setInputText(message->messageText);
        auto cursor = this->owner_->textCursor();
        cursor.movePosition(QTextCursor::End);
        this->owner_->setTextCursor(cursor);
        this->exitToInput();
    }

    MessagePtr selectedMessage() const
    {
        if (!this->active_ || this->view_.isNull() || this->blocks_.empty())
        {
            return nullptr;
        }

        auto &messages = this->view_->getMessagesSnapshot();
        const auto index = this->individualMode_
                               ? this->itemIndex_
                               : this->blocks_[this->blockIndex_].last;
        if (index >= messages.size())
        {
            return nullptr;
        }
        return messages[index]->getMessagePtr();
    }

    std::optional<std::pair<size_t, size_t>> selectedRange() const
    {
        if (!this->active_ || this->blocks_.empty() ||
            this->blockIndex_ >= this->blocks_.size())
        {
            return std::nullopt;
        }

        if (this->individualMode_)
        {
            return std::pair{this->itemIndex_, this->itemIndex_};
        }

        const auto &block = this->blocks_[this->blockIndex_];
        return std::pair{block.first, block.last};
    }

    void scrollSelectionToCenter()
    {
        if (this->view_.isNull())
        {
            return;
        }

        const auto range = this->selectedRange();
        if (!range)
        {
            return;
        }

        auto &messages = this->view_->getMessagesSnapshot();
        if (range->first >= messages.size())
        {
            return;
        }

        qreal remaining = this->view_->height() / 2.0;
        qreal relative = static_cast<qreal>(range->first);
        size_t index = range->first;

        while (remaining > 0 && index > 0)
        {
            const qreal height =
                std::max<qreal>(1, messages[index - 1]->getHeight());
            if (remaining < height)
            {
                relative = static_cast<qreal>(index - 1) +
                           (height - remaining) / height;
                remaining = 0;
            }
            else
            {
                remaining -= height;
                --index;
                relative = static_cast<qreal>(index);
            }
        }

        auto &scrollbar = this->view_->getScrollBar();
        const qreal desired = std::clamp(scrollbar.getMinimum() + relative,
                                         scrollbar.getMinimum(),
                                         scrollbar.getBottom());
        scrollbar.setDesiredValue(desired, false);
    }

    void exitToInput()
    {
        if (!this->active_)
        {
            return;
        }

        this->active_ = false;
        this->individualMode_ = false;
        this->blocks_.clear();

        if (!this->overlay_.isNull())
        {
            this->overlay_->hide();
        }

        if (!this->view_.isNull())
        {
            auto &scrollbar = this->view_->getScrollBar();
            this->view_->setEnableScrollingToBottom(
                this->scrollingToBottomBeforeSelection_);
            this->view_->unpause(PauseReason::Selection);
            this->view_->getMessagesSnapshot();
            if (this->wasAtBottomBeforeSelection_)
            {
                scrollbar.scrollToBottom(false);
            }
            else
            {
                scrollbar.setDesiredValue(
                    scrollbar.getMinimum() +
                        this->relativeScrollBeforeSelection_,
                    false);
            }
        }

        this->owner_->setFocus(Qt::ShortcutFocusReason);
        this->updateOverlay();
    }

    void resizeOverlay()
    {
        if (!this->overlay_.isNull() && !this->view_.isNull())
        {
            this->overlay_->setGeometry(this->view_->rect());
            this->overlay_->raise();
        }
    }

    void updateOverlay()
    {
        if (!this->overlay_.isNull())
        {
            this->overlay_->update();
        }
    }

    QTextEdit *owner_;
    QPointer<Split> split_;
    QPointer<ChannelView> view_;
    QPointer<MessageHistorySelectionOverlay> overlay_;
    std::vector<Block> blocks_;
    size_t blockIndex_ = 0;
    size_t itemIndex_ = 0;
    bool active_ = false;
    bool individualMode_ = false;
    bool scrollingToBottomBeforeSelection_ = true;
    bool wasAtBottomBeforeSelection_ = true;
    qreal relativeScrollBeforeSelection_ = 0;
};

inline void MessageHistorySelectionOverlay::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    const auto selection = this->controller_.selectionRect();
    if (!selection)
    {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRect(event->rect());

    auto color = this->palette().color(QPalette::Highlight);
    auto fill = color;
    fill.setAlpha(this->controller_.individualMode() ? 70 : 45);
    color.setAlpha(220);

    painter.setBrush(fill);
    painter.setPen(QPen(color, 2));
    painter.drawRoundedRect(selection->adjusted(1, 1, -1, -1), 5, 5);
}

}  // namespace chatterino
