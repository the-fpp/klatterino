// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ResizingTextEdit.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "singletons/Settings.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QObject>
#include <QSignalBlocker>
#include <QWidget>

#include <set>
#include <utility>
#include <vector>

namespace chatterino {

namespace {

constexpr size_t RECENT_MENTION_REPLY_LIMIT = 200;

SplitInput *findSplitInputParent(QWidget *widget)
{
    for (auto *parent = widget ? widget->parentWidget() : nullptr;
         parent != nullptr; parent = parent->parentWidget())
    {
        if (auto *input = qobject_cast<SplitInput *>(parent))
        {
            return input;
        }
    }
    return nullptr;
}

bool isReplyMentionCandidate(const MessagePtr &message, const ChannelPtr &channel)
{
    if (!message || !channel || !channel->isTwitchOrKickChannel())
    {
        return false;
    }

    const auto &flags = message->flags;
    if (!flags.hasAny(MessageFlag::ShowInMentions, MessageFlag::Highlighted,
                      MessageFlag::HighlightedWhisper))
    {
        return false;
    }

    if (flags.hasAny(MessageFlag::System, MessageFlag::Disabled,
                     MessageFlag::Timeout, MessageFlag::InvalidReplyTarget))
    {
        return false;
    }

    if (message->id.isEmpty())
    {
        return false;
    }

    const auto status = message->isReplyable();
    return status == Message::ReplyStatus::Replyable ||
           status == Message::ReplyStatus::ReplyableWithThread;
}

bool isOwnReplyActivity(const MessagePtr &message, const ChannelPtr &channel)
{
    if (!message || !channel || message->replyParent == nullptr)
    {
        return false;
    }

    if (!channel->isTwitchOrKickChannel())
    {
        return false;
    }

    const auto context = channel->messageSendContext();
    if (!context.authenticated || context.accountID.isEmpty() ||
        message->id.isEmpty() ||
        message->userID != context.accountID)
    {
        return false;
    }

    if (message->flags.hasAny(MessageFlag::System, MessageFlag::Disabled,
                              MessageFlag::Timeout,
                              MessageFlag::InvalidReplyTarget))
    {
        return false;
    }

    const auto status = message->isReplyable();
    return status == Message::ReplyStatus::Replyable ||
           status == Message::ReplyStatus::ReplyableWithThread;
}

bool hasExactMultiChannelProvenance(const ChannelView &view,
                                    const Message &message,
                                    const ChannelPtr &channel)
{
    const auto underlying = view.underlyingChannel();
    const auto *multi = dynamic_cast<const MultiChannel *>(underlying.get());
    if (multi == nullptr)
    {
        return true;
    }

    QStringView sourceName = message.channelName;
    if (sourceName.startsWith(u":kick:"))
    {
        sourceName = sourceName.sliced(sizeof(":kick:") - 1);
    }
    if (sourceName.startsWith(u'#'))
    {
        sourceName = sourceName.sliced(1);
    }

    for (const auto &child : multi->channels())
    {
        if (child.channel == channel &&
            multiChannelChildMatches(child, message.platform, sourceName))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

ResizingTextEdit::ResizingTextEdit()
{
    auto sizePolicy = this->sizePolicy();
    sizePolicy.setHeightForWidth(true);
    sizePolicy.setVerticalPolicy(QSizePolicy::Preferred);
    this->setSizePolicy(sizePolicy);
    this->setAcceptRichText(false);

    QObject::connect(this, &QTextEdit::textChanged, this,
                     &QWidget::updateGeometry);

    QObject::connect(this, &QTextEdit::cursorPositionChanged, [this]() {
        // If tab was pressed and we're completing/replacing the current word,
        // this code will not even be called, see ResizingTextEdit::keyPressEvent

        if (!this->completionInProgress_)
        {
            return;
        }
        qCDebug(chatterinoCommon)
            << "Finishing completion because cursor moved";
        this->completionInProgress_ = false;
    });

    // Whenever the setting for emote completion changes, force a
    // refresh on the completion model the next time "Tab" is pressed
    getSettings()->prefixOnlyEmoteCompletion.connect([this] {
        this->completionInProgress_ = false;
    });

    this->setFocusPolicy(Qt::ClickFocus);
    this->installEventFilter(this);
}

QSize ResizingTextEdit::sizeHint() const
{
    return QSize(this->width(), this->heightForWidth(this->width()));
}

bool ResizingTextEdit::hasHeightForWidth() const
{
    return true;
}

bool ResizingTextEdit::isFirstWord() const
{
    QString plainText = this->toPlainText();
    QString portionBeforeCursor = plainText.left(this->textCursor().position());
    return !portionBeforeCursor.contains(' ');
};

int ResizingTextEdit::heightForWidth(int) const
{
    auto margins = this->contentsMargins();

    return margins.top() + this->document()->size().height() +
           margins.bottom() + 5;
}

QString ResizingTextEdit::textUnderCursor(bool *hadSpace) const
{
    auto currentText = this->toPlainText();

    QTextCursor tc = this->textCursor();

    auto textUpToCursor = currentText.left(tc.selectionStart());

    auto words = QStringView{textUpToCursor}.split(' ');
    if (words.size() == 0)
    {
        return QString();
    }

    bool first = true;
    QString lastWord;
    for (auto it = words.crbegin(); it != words.crend(); ++it)
    {
        auto word = *it;

        if (first && word.isEmpty())
        {
            first = false;
            if (hadSpace != nullptr)
            {
                *hadSpace = true;
            }
            continue;
        }

        lastWord = word.toString();
        break;
    }

    if (lastWord.isEmpty())
    {
        return QString();
    }

    return lastWord;
}

bool ResizingTextEdit::eventFilter(QObject *obj, QEvent *event)
{
    (void)obj;  // unused

    // makes QShortcuts work in the ResizingTextEdit
    if (event->type() != QEvent::ShortcutOverride)
    {
        return false;
    }
    auto *ev = static_cast<QKeyEvent *>(event);
    ev->ignore();
    if ((ev->key() == Qt::Key_C || ev->key() == Qt::Key_Insert) &&
        ev->modifiers() == Qt::ControlModifier)
    {
        return false;
    }
    return true;
}

void ResizingTextEdit::keyPressEvent(QKeyEvent *event)
{
    event->ignore();

    this->keyPressed.invoke(event);

    const bool isReplyBackspace =
        !event->isAccepted() && event->key() == Qt::Key_Backspace &&
        (event->modifiers() == Qt::NoModifier ||
         event->modifiers() == Qt::ControlModifier);
    if (isReplyBackspace)
    {
        if (auto *input = findSplitInputParent(this))
        {
            if (input->handleEmptyBackspaceReply(
                    event->modifiers() == Qt::ControlModifier))
            {
                event->accept();
                return;
            }
        }
    }

    if (!event->isAccepted() && event->key() == Qt::Key_Escape &&
        event->modifiers() == Qt::NoModifier)
    {
        if (auto *input = findSplitInputParent(this))
        {
            if (input->enableInlineReplying_ && input->replyTarget_ != nullptr)
            {
                input->setReply(nullptr, {});
                input->editTextChanged();
                event->accept();
                return;
            }
        }
    }

    bool doComplete =
        this->tabCompletionEnabled_ &&
        (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
        (event->modifiers() & Qt::ControlModifier) == Qt::NoModifier &&
        !event->isAccepted();

    if (doComplete)
    {
        // check if there is a completer
        if (!this->completer_)
        {
            return;
        }

        QString currentCompletion = this->textUnderCursor();

        // check if there is something to complete
        if (currentCompletion.size() <= 1)
        {
            return;
        }

        // always expected to be TabCompletionModel
        auto *completionModel =
            dynamic_cast<TabCompletionModel *>(this->completer_->model());
        assert(completionModel != nullptr);

        auto applyCurrentCompletion = [this]() {
            const auto completion = this->completer_->currentCompletion();
            if (completion.isEmpty())
            {
                return false;
            }

            {
                // this blocks cursor movement events from resetting tab completion
                QSignalBlocker dontTriggerCursorMovement(this);
                this->insertCompletion(completion);
            }
            this->completionInProgress_ = true;
            this->textChanged();
            return true;
        };

        if (!this->completionInProgress_)
        {
            // First type pressing tab after modifying a message, we refresh our
            // completion model
            this->completer_->setModel(completionModel);
            const auto draft = this->messageDraftProvider_
                                   ? this->messageDraftProvider_()
                                   : MessageDraft::fromPlainText(
                                         this->toPlainText());
            completionModel->updateResults(currentCompletion, draft,
                                           this->textCursor().position(),
                                           this->isFirstWord());
            this->completionInProgress_ = true;

            if (!this->completer_->setCurrentRow(0))
            {
                this->completionInProgress_ = false;
                return;
            }
            applyCurrentCompletion();
            return;
        }

        // scrolling through selections
        if (event->key() == Qt::Key_Tab)
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() + 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(0);
            }
        }
        else
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() - 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(
                    this->completer_->completionCount() - 1);
            }
        }

        applyCurrentCompletion();
        return;
    }

    if (!event->text().isEmpty())
    {
        this->completionInProgress_ = false;
    }

    if (!event->isAccepted())
    {
        QTextEdit::keyPressEvent(event);
    }
}

bool SplitInput::handleEmptyBackspaceReply(bool reverse)
{
    const auto text = this->ui_.textEdit->toPlainText().trimmed();
    if (!text.isEmpty())
    {
        if (!this->enableInlineReplying_ || this->replyTarget_ == nullptr)
        {
            return false;
        }

        const auto replyPrefix = "@" + this->replyTarget_->displayName;
        if (text != replyPrefix)
        {
            return false;
        }
    }

    return this->replyToRecentMentionOrCycle(RECENT_MENTION_REPLY_LIMIT,
                                             reverse);
}

bool SplitInput::replyToRecentMentionOrCycle(size_t limit, bool reverse)
{
    if (!this->enableInlineReplying_ || this->split_ == nullptr ||
        this->channelView_ == nullptr)
    {
        return false;
    }

    auto channel = this->split_->getChannel();
    if (!channel)
    {
        return false;
    }

    struct Candidate {
        MessagePtr message;
        ChannelPtr channel;
    };
    std::vector<Candidate> candidates;
    std::set<std::pair<ChannelPtr, QString>> seenTargets;

    const auto messages = channel->getMessageSnapshot(limit);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        const auto &message = *it;
        if (!message)
        {
            continue;
        }

        auto activityChannel = this->channelView_->inferChannel(*message);
        if (isReplyMentionCandidate(message, activityChannel) &&
            hasExactMultiChannelProvenance(*this->channelView_, *message,
                                           activityChannel))
        {
            if (seenTargets.emplace(activityChannel, message->id).second)
            {
                candidates.push_back({message, std::move(activityChannel)});
            }
            continue;
        }

        if (!isOwnReplyActivity(message, activityChannel) ||
            !hasExactMultiChannelProvenance(*this->channelView_, *message,
                                            activityChannel))
        {
            continue;
        }

        // Outgoing replies make their immediate parent recent. Re-infer the
        // parent independently so a malformed cross-channel association can
        // never redirect a multi-channel reply.
        auto target = message->replyParent;
        auto targetChannel = this->channelView_->inferChannel(*target);
        if (!targetChannel || targetChannel != activityChannel ||
            !hasExactMultiChannelProvenance(*this->channelView_, *target,
                                            targetChannel))
        {
            continue;
        }

        const auto targetContext = targetChannel->messageSendContext();
        if (targetContext.accountID.isEmpty() ||
            target->userID.isEmpty() ||
            target->userID == targetContext.accountID)
        {
            continue;
        }

        const auto status = target->isReplyable();
        if (target->id.isEmpty() ||
            target->flags.hasAny(MessageFlag::System, MessageFlag::Disabled,
                                 MessageFlag::Timeout,
                                 MessageFlag::InvalidReplyTarget) ||
            (status != Message::ReplyStatus::Replyable &&
             status != Message::ReplyStatus::ReplyableWithThread) ||
            !seenTargets.emplace(targetChannel, target->id).second)
        {
            continue;
        }

        candidates.push_back({std::move(target), std::move(targetChannel)});
    }

    if (candidates.empty())
    {
        return false;
    }

    qsizetype currentIndex = -1;
    if (this->replyTarget_)
    {
        for (qsizetype i = 0; i < static_cast<qsizetype>(candidates.size()); ++i)
        {
            if (candidates[static_cast<size_t>(i)].message->id ==
                    this->replyTarget_->id &&
                candidates[static_cast<size_t>(i)].channel ==
                    this->replyChannel_.lock())
            {
                currentIndex = i;
                break;
            }
        }
    }

    if (currentIndex >= 0)
    {
        qsizetype nextIndex = currentIndex + (reverse ? -1 : 1);
        if (nextIndex < 0 || nextIndex >= static_cast<qsizetype>(candidates.size()))
        {
            this->setReply(nullptr, {});
            this->editTextChanged();
            return true;
        }

        const auto &candidate = candidates[static_cast<size_t>(nextIndex)];
        this->setReply(candidate.message, candidate.channel);
        return true;
    }

    const auto &candidate = reverse ? candidates.back() : candidates.front();
    this->setReply(candidate.message, candidate.channel);
    return true;
}

void ResizingTextEdit::focusInEvent(QFocusEvent *event)
{
    QTextEdit::focusInEvent(event);

    if (event->gotFocus())
    {
        this->focused.invoke();
    }
}

void ResizingTextEdit::focusOutEvent(QFocusEvent *event)
{
    QTextEdit::focusOutEvent(event);

    if (event->lostFocus())
    {
        this->focusLost.invoke();
    }
}

void ResizingTextEdit::setCompleter(QCompleter *c)
{
    delete this->completer_;

    this->completer_ = c;

    if (!this->completer_)
    {
        return;
    }

    this->completer_->setWidget(this);
    this->completer_->setCompletionMode(QCompleter::PopupCompletion);
    this->completer_->setCaseSensitivity(Qt::CaseInsensitive);

    QObject::connect(this->completer_,
                     QOverload<const QString &>::of(&QCompleter::activated),
                     this, &ResizingTextEdit::insertCompletion);
}

void ResizingTextEdit::setTabCompletionEnabled(bool enabled)
{
    this->tabCompletionEnabled_ = enabled;
    if (!enabled)
    {
        this->completionInProgress_ = false;
    }
}

void ResizingTextEdit::setMessageDraftProvider(
    std::function<MessageDraft()> provider)
{
    this->messageDraftProvider_ = std::move(provider);
}

void ResizingTextEdit::insertCompletion(const QString &completion)
{
    TabCompletionModel::Selection selection;
    if (this->completer_)
    {
        if (const auto *model =
                dynamic_cast<const TabCompletionModel *>(
                    this->completer_->model()))
        {
            selection = model->selectionForCompletion(completion);
        }
    }

    bool hadSpace = false;
    const auto currentCompletion = this->textUnderCursor(&hadSpace);

    auto cursor = this->textCursor();
    if (!currentCompletion.isEmpty())
    {
        cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor,
                            currentCompletion.size() + (hadSpace ? 1 : 0));
    }
    cursor.insertText(completion);
    const auto insertedStart = cursor.position() - completion.size();
    this->setTextCursor(cursor);
    this->completionInProgress_ = false;

    if (selection.emote)
    {
        const auto offset = completion.indexOf(selection.emote->insertionText);
        if (offset >= 0)
        {
            this->emoteCompletionInserted.invoke(insertedStart + offset,
                                                  *selection.emote);
        }
        else
        {
            const auto unresolved = QStringView{completion}.trimmed();
            const auto unresolvedOffset =
                QStringView{completion}.indexOf(unresolved);
            this->unresolvedEmoteCompletionInserted.invoke(
                insertedStart + unresolvedOffset, unresolved.size());
        }
    }
    else if (selection.provenanceAmbiguous)
    {
        const auto unresolved = QStringView{completion}.trimmed();
        const auto offset = QStringView{completion}.indexOf(unresolved);
        this->unresolvedEmoteCompletionInserted.invoke(
            insertedStart + offset, unresolved.size());
    }
}

void ResizingTextEdit::resetCompletion()
{
    this->completionInProgress_ = false;
}

bool ResizingTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    if (source->hasImage())
    {
        return true;
    }
    return QTextEdit::canInsertFromMimeData(source);
}

void ResizingTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (source->hasImage())
    {
        this->imagePasted.invoke(source);
        return;
    }
    QTextEdit::insertFromMimeData(source);
}

void ResizingTextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    auto *menu = createStandardContextMenu(event->pos());
    this->contextMenuRequested.invoke(menu, event->pos());
    menu->exec(event->globalPos());
    delete menu;
}

}  // namespace chatterino
