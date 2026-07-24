// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "Application.hpp"
#include "common/enums/MessageOverflow.hpp"
#include "common/QLogging.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/MultiChannel.hpp"
#include "util/PostToThread.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CmdDeleteKeyFilter.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/InputHighlighter.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QCompleter>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTimer>
#include <qwindow.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <ranges>
#include <set>
#include <utility>

using namespace Qt::Literals;

namespace chatterino {

namespace {

class AutomaticRoutingIndicator final : public QWidget
{
public:
    explicit AutomaticRoutingIndicator(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(
            QStringLiteral("multiChannelAutomaticRoutingIndicator"));
        this->setAccessibleName(QStringLiteral("Automatic routing indicator"));
        this->setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto color = this->palette().color(QPalette::Text);
        color.setAlphaF(this->isEnabled() ? 0.9 : 0.45);
        const auto stroke = std::max<qreal>(1.25, this->width() / 10.0);
        painter.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(color);

        const QRectF bounds =
            QRectF(this->rect()).adjusted(stroke, stroke * 2, -stroke,
                                           -stroke * 2);
        const auto sourceX = bounds.left();
        const auto branchX = bounds.left() + bounds.width() * 0.42;
        const auto destinationX = bounds.right() - stroke;
        const auto top = bounds.top() + stroke;
        const auto middle = bounds.center().y();
        const auto bottom = bounds.bottom() - stroke;

        painter.drawLine(QPointF(sourceX, middle), QPointF(branchX, middle));
        painter.drawLine(QPointF(branchX, top), QPointF(branchX, bottom));
        painter.drawLine(QPointF(branchX, top), QPointF(destinationX, top));
        painter.drawLine(QPointF(branchX, middle),
                         QPointF(destinationX, middle));
        painter.drawLine(QPointF(branchX, bottom),
                         QPointF(destinationX, bottom));

        const auto radius = std::max<qreal>(1.25, stroke * 0.85);
        painter.setPen(Qt::NoPen);
        for (const auto y : {top, middle, bottom})
        {
            painter.drawEllipse(QPointF(destinationX, y), radius, radius);
        }
    }
};

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

bool isChannelBoundInput(QStringView input)
{
    const auto trimmed = input.trimmed();
    return trimmed.startsWith(u'/') ||
           (trimmed.startsWith(u'.') && !trimmed.startsWith(u".."));
}

QString multiChannelPlatformID(MultiChannel::Platform platform)
{
    switch (platform)
    {
        case MultiChannel::Platform::Twitch:
            return QStringLiteral("twitch");
        case MultiChannel::Platform::Kick:
            return QStringLiteral("kick");
        case MultiChannel::Platform::Rumble:
            return QStringLiteral("rumble");
    }
    Q_UNREACHABLE();
}

QString multiChannelPlatformName(MultiChannel::Platform platform)
{
    switch (platform)
    {
        case MultiChannel::Platform::Twitch:
            return QStringLiteral("Twitch");
        case MultiChannel::Platform::Kick:
            return QStringLiteral("Kick");
        case MultiChannel::Platform::Rumble:
            return QStringLiteral("Rumble");
    }
    Q_UNREACHABLE();
}

MessagePlatform messagePlatform(MultiChannel::Platform platform)
{
    switch (platform)
    {
        case MultiChannel::Platform::Twitch:
            return MessagePlatform::AnyOrTwitch;
        case MultiChannel::Platform::Kick:
            return MessagePlatform::Kick;
        case MultiChannel::Platform::Rumble:
            return MessagePlatform::Rumble;
    }
    Q_UNREACHABLE();
}

bool isReplyTagCharacter(QChar character)
{
    return character.isLetterOrNumber() || character == u'_' ||
           character == u'-' || character == u'.';
}

std::optional<qsizetype> replyTagLength(QStringView text)
{
    if (text.size() < 2 || text[0] != u'@' || !isReplyTagCharacter(text[1]))
    {
        return std::nullopt;
    }

    qsizetype length = 2;
    while (length < text.size() && isReplyTagCharacter(text[length]))
    {
        ++length;
    }
    if (length < text.size() && text[length] == u',')
    {
        ++length;
    }
    if (length < text.size() && !text[length].isSpace())
    {
        return std::nullopt;
    }
    return length;
}

QString stripBoundaryReplyTags(QString text)
{
    text = text.trimmed();

    while (!text.isEmpty())
    {
        const auto length = replyTagLength(text);
        if (!length)
        {
            break;
        }
        text.remove(0, *length);
        text = text.trimmed();
    }

    while (!text.isEmpty())
    {
        qsizetype start = text.size();
        while (start > 0 && !text[start - 1].isSpace())
        {
            --start;
        }

        const auto candidate = QStringView{text}.sliced(start);
        const auto length = replyTagLength(candidate);
        if (!length || *length != candidate.size())
        {
            break;
        }
        text.truncate(start);
        text = text.trimmed();
    }

    return text;
}

MessageDraft rebaseDraftAfterPrefixRemoval(MessageDraft draft,
                                           qsizetype removedPrefix,
                                           QString resultingText)
{
    if (removedPrefix < 0 || removedPrefix > draft.text.size())
    {
        draft.provenanceValid = false;
        draft.emotes.clear();
        draft.text = std::move(resultingText);
        return draft;
    }

    std::vector<DraftEmoteOccurrence> rebased;
    rebased.reserve(draft.emotes.size());
    for (auto occurrence : draft.emotes)
    {
        if (occurrence.start < removedPrefix)
        {
            // The reply prefix must be plain UI text. If a tracked range
            // overlaps it, do not transfer that identity into the payload.
            draft.provenanceValid = false;
            continue;
        }
        occurrence.start -= removedPrefix;
        rebased.push_back(std::move(occurrence));
    }
    draft.emotes = std::move(rebased);
    draft.text = std::move(resultingText);
    return draft;
}

struct RetainedDraftSlice {
    QString text;
    qsizetype sourceStart = 0;
    qsizetype sourceEnd = 0;

    explicit RetainedDraftSlice(QString source)
        : text(std::move(source))
        , sourceEnd(this->text.size())
    {
    }

    void trim()
    {
        qsizetype first = 0;
        while (first < this->text.size() && this->text[first].isSpace())
        {
            ++first;
        }

        auto last = this->text.size();
        while (last > first && this->text[last - 1].isSpace())
        {
            --last;
        }

        this->sourceStart += first;
        this->sourceEnd = this->sourceStart + (last - first);
        this->text = this->text.sliced(first, last - first);
    }

    void removePrefix(qsizetype length)
    {
        length = std::clamp<qsizetype>(length, 0, this->text.size());
        this->text.remove(0, length);
        this->sourceStart += length;
    }
};

MessageDraft rebaseDraftToRetainedSlice(MessageDraft draft,
                                        const RetainedDraftSlice &slice,
                                        qsizetype insertedPrefixLength,
                                        QString resultingText)
{
    std::vector<DraftEmoteOccurrence> rebased;
    rebased.reserve(draft.emotes.size());
    for (auto occurrence : draft.emotes)
    {
        const auto occurrenceEnd = occurrence.start + occurrence.length;
        if (occurrenceEnd <= slice.sourceStart ||
            occurrence.start >= slice.sourceEnd)
        {
            // This occurrence was wholly removed with an old reply prefix or
            // surrounding whitespace. It cannot affect the resulting draft.
            continue;
        }
        if (occurrence.start < slice.sourceStart ||
            occurrenceEnd > slice.sourceEnd)
        {
            // Only a portion survived the rewrite, so retaining its provider
            // identity would be unsafe even if the rendered text looks equal.
            draft.provenanceValid = false;
            continue;
        }

        occurrence.start = insertedPrefixLength + occurrence.start -
                           slice.sourceStart;
        if (occurrence.start < 0 || occurrence.length <= 0 ||
            occurrence.start > resultingText.size() ||
            occurrence.length > resultingText.size() - occurrence.start ||
            resultingText.sliced(occurrence.start, occurrence.length) !=
                occurrence.insertionText)
        {
            draft.provenanceValid = false;
            continue;
        }
        rebased.push_back(std::move(occurrence));
    }
    draft.emotes = std::move(rebased);
    draft.text = std::move(resultingText);
    return draft;
}

MessageDraft truncateDraft(MessageDraft draft, qsizetype limit)
{
    limit = std::clamp<qsizetype>(limit, 0, draft.text.size());
    const auto resultingText = draft.text.left(limit);
    std::vector<DraftEmoteOccurrence> retained;
    retained.reserve(draft.emotes.size());
    for (auto occurrence : draft.emotes)
    {
        if (occurrence.start < 0 || occurrence.length <= 0 ||
            occurrence.start > draft.text.size() ||
            occurrence.length > draft.text.size() - occurrence.start)
        {
            draft.provenanceValid = false;
            continue;
        }

        const auto occurrenceEnd = occurrence.start + occurrence.length;
        if (occurrence.start >= limit)
        {
            // The selected token was removed completely.
            continue;
        }
        if (occurrenceEnd > limit)
        {
            // A partial token must not become untracked plain text.
            draft.provenanceValid = false;
            continue;
        }
        retained.push_back(std::move(occurrence));
    }
    draft.emotes = std::move(retained);
    draft.text = resultingText;
    return draft;
}

void restoreDraftTracking(MessageDraftTracker &tracker,
                          const MessageDraft &draft)
{
    tracker.clear();
    for (const auto &occurrence : draft.emotes)
    {
        tracker.recordSelection(
            occurrence.start,
            {
                .identity = occurrence.identity,
                .insertionText = occurrence.insertionText,
                .availability = occurrence.availability,
                .availabilityAlternatives =
                    occurrence.availabilityAlternatives,
                .identityAlternatives = occurrence.identityAlternatives,
            },
            draft.text);
    }
    if (!draft.provenanceValid)
    {
        // A zero-length unresolved range intentionally sets the tracker's
        // invalid-insertion bit for an empty document.
        tracker.recordUnresolvedSelection(0, draft.text.size(), draft.text);
    }
}

QString replyDraftFailure(const Channel &destination,
                          const MessageDraftEvaluation &evaluation)
{
    QStringList reasons;
    for (const auto &rejection : evaluation.rejections)
    {
        if (!reasons.contains(rejection.detail))
        {
            reasons.push_back(rejection.detail);
        }
    }
    if (reasons.empty())
    {
        reasons.push_back(QStringLiteral("not currently available"));
    }
    return QStringLiteral("Reply not sent — %1: %2")
        .arg(destination.getDisplayName(), reasons.join(QStringLiteral(" ")));
}

QString ordinaryDraftFailure(const Channel &destination,
                             const MessageDraftEvaluation &evaluation)
{
    QStringList reasons;
    for (const auto &rejection : evaluation.rejections)
    {
        if (!reasons.contains(rejection.detail))
        {
            reasons.push_back(rejection.detail);
        }
    }
    if (reasons.empty())
    {
        reasons.push_back(QStringLiteral("not currently available"));
    }
    return QStringLiteral("Message not sent — %1: %2")
        .arg(destination.getDisplayName(), reasons.join(QStringLiteral(" ")));
}

QString draftRoutingFailure(
    const MultiChannel &multi,
    const MultiChannel::DraftDispatchResult &result,
    std::optional<size_t> onlyChild = std::nullopt)
{
    QStringList reasons;
    const auto children = multi.channels();
    for (size_t index = 0; index < result.evaluations.size(); ++index)
    {
        if (onlyChild && index != *onlyChild)
        {
            continue;
        }
        const auto &evaluation = result.evaluations[index];
        if (evaluation.sendable)
        {
            continue;
        }
        const auto name = index < children.size()
                              ? children[index].channel->getDisplayName()
                              : QStringLiteral("destination %1")
                                    .arg(static_cast<qulonglong>(index + 1));
        const auto reason = evaluation.rejections.empty()
                                ? QStringLiteral("not currently available")
                                : evaluation.rejections.front().detail;
        reasons.push_back(QStringLiteral("%1: %2").arg(name, reason));
    }
    if (reasons.empty())
    {
        reasons.push_back(QStringLiteral("no permitted destination"));
    }
    return QStringLiteral("Message not sent — %1").arg(reasons.join("; "));
}

class BackwardsSearchLineEdit : public QLineEdit
{
    Q_OBJECT

public:
    BackwardsSearchLineEdit(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
    }

Q_SIGNALS:
    void onFocusOut();

protected:
    void focusOutEvent(QFocusEvent * /* event */) override
    {
        this->onFocusOut();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        auto key = event->key();
        bool isEndKey = key == Qt::Key_Escape || key == Qt::Key_Enter ||
                        key == Qt::Key_Tab || key == Qt::Key_Backtab;
        if (!event->modifiers().testFlag(Qt::ControlModifier) && isEndKey)
        {
            this->clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

}  // namespace

SplitInput::SplitInput(Split *_chatWidget, bool enableInlineReplying)
    : SplitInput(_chatWidget, _chatWidget, _chatWidget->view_,
                 enableInlineReplying)
{
}

SplitInput::SplitInput(QWidget *parent, Split *_chatWidget,
                       ChannelView *_channelView, bool enableInlineReplying)
    : BaseWidget(parent)
    , split_(_chatWidget)
    , channelView_(_channelView)
    , enableInlineReplying_(enableInlineReplying)
    , backgroundColorAnimation(this, "backgroundColor"_ba)
{
    this->installEventFilter(this);
    this->initLayout();

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    auto *spellChecker = getApp()->getSpellChecker();
    this->inputHighlighter = new InputHighlighter(*spellChecker, this);
    this->updateChannel();

    this->signalHolder_.managedConnect(this->split_->channelChanged, [this] {
        this->updateChannel();
    });

    getSettings()->enableSpellChecking.connect(
        [this] {
            this->checkSpellingChanged();
        },
        this->signalHolder_);

    // misc
    this->installTextEditEvents();
    this->addShortcuts();
    // The textEdit's signal will be destroyed before this SplitInput is
    // destroyed, so we can safely ignore this signal's connection.
    std::ignore = this->ui_.textEdit->focusLost.connect([this] {
        this->hideCompletionPopup();
    });
    this->scaleChangedEvent(this->scale());
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    QEasingCurve curve;
    curve.setCustomType(highlightEasingFunction);
    this->backgroundColorAnimation.setDuration(500);
    this->backgroundColorAnimation.setEasingCurve(curve);
}

void SplitInput::initLayout()
{
    auto *app = getApp();
    LayoutCreator<SplitInput> layoutCreator(this);

    auto layout =
        layoutCreator.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.vbox);
    layout->setSpacing(0);
    this->applyOuterMargin();

    // backwards ui
    {
        auto wrap =
            layout.emplace<QWidget>().assign(&this->ui_.historySearchWrap);
        wrap->setVisible(false);
        wrap->setAutoFillBackground(true);
        auto palette = wrap->palette();
        palette.setColor(QPalette::Base, Qt::transparent);
        palette.setColor(QPalette::Window, getTheme()->splits.input.background);
        wrap->setPalette(palette);

        auto backLayout = wrap.setLayoutType<QHBoxLayout>().withoutMargin();

        backLayout->addSpacing(5);
        auto input = backLayout.emplace<BackwardsSearchLineEdit>().assign(
            &this->ui_.historySearchInput);
        input->setFrame(false);
        input->setPlaceholderText("Search input history...");
        input->setFocusPolicy(Qt::ClickFocus);
        QObject::connect(input.getElement(), &QLineEdit::textChanged, this,
                         [this](const QString &text) {
                             if (this->inHistorySearch)
                             {
                                 this->historySearchQuery = text;
                                 this->refreshHistorySearch(
                                     this->lastHistorySearchBackwards,
                                     this->lastHistorySearchLoop);
                             }
                         });
        QObject::connect(input.getElement(),
                         &BackwardsSearchLineEdit::onFocusOut, this,
                         &SplitInput::stopHistorySearchIfNecessary);
        backLayout->setStretch(0, 1);
        backLayout->addSpacing(5);

        auto label =
            backLayout.emplace<QLabel>().assign(&this->ui_.historySearchLabel);
        label->setFrameStyle(QFrame::NoFrame);
        backLayout->addSpacing(5);
    }

    // reply label stuff
    auto replyWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.replyWrapper);
    replyWrapper->setContentsMargins(0, 0, 1, 1);

    auto replyVbox =
        replyWrapper.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.replyVbox);
    replyVbox->setSpacing(1);

    auto replyHbox =
        replyVbox.emplace<QHBoxLayout>().assign(&this->ui_.replyHbox);

    auto *messageVbox = new QVBoxLayout;
    this->ui_.replyMessage = new MessageView();
    messageVbox->addWidget(this->ui_.replyMessage, 0, Qt::AlignLeft);
    messageVbox->setContentsMargins(10, 0, 0, 0);
    replyVbox->addLayout(messageVbox, 0);

    auto replyLabel = replyHbox.emplace<QLabel>().assign(&this->ui_.replyLabel);
    replyLabel->setAlignment(Qt::AlignLeft);
    replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    replyHbox->addStretch(1);

    auto replyCancelButton = replyHbox
                                 .emplace<SvgButton>(
                                     SvgButton::Src{
                                         .dark = ":/buttons/cancel.svg",
                                         .light = ":/buttons/cancelDark.svg",
                                     },
                                     nullptr, QSize{4, 0})
                                 .assign(&this->ui_.cancelReplyButton);

    replyCancelButton->hide();
    replyLabel->hide();

    auto inputWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.inputWrapper);
    inputWrapper->setContentsMargins(1, 1, 1, 1);

    // hbox for input, right box
    auto hboxLayout =
        inputWrapper.setLayoutType<QHBoxLayout>().withoutMargin().assign(
            &this->ui_.inputHbox);

    auto automaticRoutingIndicator =
        hboxLayout.emplace<AutomaticRoutingIndicator>().assign(
            &this->ui_.automaticRoutingIndicator);
    auto automaticRoutingPalette = automaticRoutingIndicator->palette();
    automaticRoutingPalette.setColor(QPalette::Text,
                                     this->theme->splits.input.text);
    automaticRoutingIndicator->setPalette(automaticRoutingPalette);
    automaticRoutingIndicator->hide();

    auto routingPlatformButton = hboxLayout.emplace<PixmapButton>().assign(
        &this->ui_.routingPlatformButton);
    routingPlatformButton->setObjectName(
        QStringLiteral("multiChannelRoutingPlatformButton"));
    routingPlatformButton->setAccessibleName(
        QStringLiteral("Multi-channel destination platform"));
    routingPlatformButton->setFocusPolicy(Qt::StrongFocus);
    routingPlatformButton->setMarginEnabled(true);
    routingPlatformButton->installEventFilter(this);
    routingPlatformButton->hide();
    auto *routingTemporaryMarker = new QLabel(routingPlatformButton.getElement());
    routingTemporaryMarker->setText(QStringLiteral("1"));
    routingTemporaryMarker->setAlignment(Qt::AlignCenter);
    routingTemporaryMarker->setAttribute(Qt::WA_TransparentForMouseEvents);
    routingTemporaryMarker->setAutoFillBackground(true);
    routingTemporaryMarker->hide();
    this->ui_.routingTemporaryMarker = routingTemporaryMarker;

    // input
    auto textEdit =
        hboxLayout.emplace<ResizingTextEdit>().assign(&this->ui_.textEdit);
    textEdit->setTabCompletionEnabled(false);
    connect(textEdit.getElement(), &ResizingTextEdit::textChanged, this,
            &SplitInput::editTextChanged);
    textEdit->setMessageDraftProvider([this] {
        const auto text = this->ui_.textEdit->toPlainText();
        return this->currentMessageDraft(text);
    });
    textEdit->setFrameStyle(QFrame::NoFrame);
    this->trackedDocumentText_ = textEdit->toPlainText();
    this->trackedDocumentRichText_ = textEdit->document()->toHtml();
    QObject::connect(textEdit->document(), &QTextDocument::contentsChange,
                     this, [this](int position, int removed, int added) {
                         const auto currentText =
                             this->ui_.textEdit->toPlainText();
                         const auto currentRichText =
                             this->ui_.textEdit->document()->toHtml();
                         if (currentText == this->trackedDocumentText_ &&
                             ((removed == 0 && added == 0) ||
                              currentRichText !=
                                  this->trackedDocumentRichText_))
                         {
                             // QTextDocument can report formatting either as
                             // a zero-length dirty notification or as a
                             // remove/add replacement (which can include its
                             // paragraph separator). A changed rich snapshot
                             // with unchanged plain text is formatting-only;
                             // syntax-highlighter layout formats need not be
                             // represented by toHtml().
                             this->trackedDocumentRichText_ = currentRichText;
                             return;
                         }
                         this->draftTracker_.contentsChanged(
                             position, removed, added, currentText);
                         this->trackedDocumentText_ = currentText;
                         this->trackedDocumentRichText_ = currentRichText;
                     });
    std::ignore = textEdit->emoteCompletionInserted.connect(
        [this](qsizetype start, const DraftEmoteCandidate &candidate) {
            this->draftTracker_.recordSelection(
                start, candidate, this->ui_.textEdit->toPlainText());
            this->updateRoutingPlatformButton();
        });
    std::ignore = textEdit->unresolvedEmoteCompletionInserted.connect(
        [this](qsizetype start, qsizetype length) {
            this->draftTracker_.recordUnresolvedSelection(
                start, length, this->ui_.textEdit->toPlainText());
            this->updateRoutingPlatformButton();
        });

    auto *shortcutFilter = new CmdDeleteKeyFilter(this);
    textEdit->installEventFilter(shortcutFilter);

    hboxLayout.emplace<LabelButton>("SEND").assign(&this->ui_.sendButton);
    this->ui_.sendButton->hide();

    QObject::connect(this->ui_.sendButton, &Button::leftClicked, [this] {
        std::vector<QString> arguments;
        this->handleSendMessage(arguments);
    });

    getSettings()->showSendButton.connect(
        [this](const bool value, auto) {
            if (value)
            {
                this->ui_.sendButton->show();
            }
            else
            {
                this->ui_.sendButton->hide();
            }
        },
        this->managedConnections_);

    // right box
    auto box = hboxLayout.emplace<QVBoxLayout>().withoutMargin();
    box->setSpacing(0);
    {
        auto hbox = box.emplace<QHBoxLayout>().withoutMargin();
        this->ui_.textEditLength = new QLabel();
        // Right-align the labels contents
        this->ui_.textEditLength->setAlignment(Qt::AlignRight);
        hbox->addWidget(this->ui_.textEditLength);

        this->ui_.sendWaitStatus = new QLabel();
        this->ui_.sendWaitStatus->setAlignment(Qt::AlignRight);
        this->ui_.sendWaitStatus->setHidden(true);
        hbox->addWidget(this->ui_.sendWaitStatus);

        this->ui_.emoteButton = new SvgButton(
            {
                .dark = ":/buttons/emote.svg",
                .light = ":/buttons/emoteDark.svg",
            },
            nullptr, QSize{6, 3});
        box->addWidget(this->ui_.emoteButton, 0, Qt::AlignRight);
    }

    // ---- misc

    // set edit font
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));
    QObject::connect(this->ui_.textEdit, &QTextEdit::cursorPositionChanged,
                     this, &SplitInput::onCursorPositionChanged);
    QObject::connect(this->ui_.textEdit, &QTextEdit::textChanged, this,
                     &SplitInput::onTextChanged);

    this->managedConnections_.managedConnect(app->getFonts()->fontChanged,
                                             [this] {
                                                 this->updateFonts();
                                             });

    // open emote popup
    QObject::connect(this->ui_.emoteButton, &Button::leftClicked, [this] {
        this->openEmotePopup();
    });

    // clear input and remove reply thread
    QObject::connect(this->ui_.cancelReplyButton, &Button::leftClicked, [this] {
        this->setReply(nullptr, {});
    });

    // Forward selection change signal
    QObject::connect(this->ui_.textEdit, &QTextEdit::copyAvailable,
                     [this](bool available) {
                         if (available)
                         {
                             this->selectionChanged.invoke();
                         }
                     });

    // textEditLength visibility
    getSettings()->showMessageLength.connect(
        [this](const bool &value, auto) {
            // this->ui_.textEditLength->setHidden(!value);
            this->editTextChanged();
        },
        this->managedConnections_);

    // sendWaitStatus visibility
    getSettings()->showSendWaitTimer.connect(
        [this](bool value, const auto &) {
            if (!this->ui_.sendWaitStatus->text().isEmpty())
            {
                this->ui_.sendWaitStatus->setHidden(!value);
            }
        },
        this->managedConnections_);
}

void SplitInput::triggerSelfMessageReceived()
{
    if (this->backgroundColorAnimation.state() != QPropertyAnimation::Stopped)
    {
        this->backgroundColorAnimation.stop();
    }
    this->backgroundColorAnimation.setDirection(QPropertyAnimation::Forward);
    this->backgroundColorAnimation.start();
}

void SplitInput::scaleChangedEvent(float scale)
{
    // update the icon size of the buttons
    this->updateEmoteButton();
    this->updateCancelReplyButton();
    const auto routingButtonSize =
        std::max(18, static_cast<int>(std::round(26 * scale)));
    const auto routingIndicatorWidth =
        std::max(12, static_cast<int>(std::round(14 * scale)));
    this->ui_.automaticRoutingIndicator->setFixedSize(routingIndicatorWidth,
                                                      routingButtonSize);
    this->ui_.routingPlatformButton->setFixedSize(routingButtonSize,
                                                  routingButtonSize);
    const auto markerSize =
        std::max(8, static_cast<int>(std::round(10 * scale)));
    this->ui_.routingTemporaryMarker->setFixedSize(markerSize, markerSize);
    this->ui_.routingTemporaryMarker->move(
        routingButtonSize - markerSize, 0);
    this->ui_.routingTemporaryMarker->setFont(
        getApp()->getFonts()->getFont(FontStyle::ChatSmall, scale));
    this->updateRoutingPlatformButton();

    // set maximum height
    if (!this->hidden)
    {
        this->setMaximumHeight(this->scaledMaxHeight());
        if (this->replyTarget_ != nullptr)
        {
            this->ui_.vbox->setSpacing(this->marginForTheme());
        }
    }
    this->updateFonts();
}

void SplitInput::themeChangedEvent()
{
    {
        QPalette palette;
        palette.setColor(QPalette::WindowText, this->theme->splits.input.text);
        this->ui_.textEditLength->setPalette(palette);
        this->ui_.sendWaitStatus->setPalette(palette);
    }
    {
        auto palette = this->ui_.automaticRoutingIndicator->palette();
        palette.setColor(QPalette::Text, this->theme->splits.input.text);
        this->ui_.automaticRoutingIndicator->setPalette(palette);
    }
    {
        auto palette = this->ui_.routingTemporaryMarker->palette();
        palette.setColor(QPalette::Window,
                         this->theme->splits.input.background);
        palette.setColor(QPalette::WindowText, this->theme->splits.input.text);
        this->ui_.routingTemporaryMarker->setPalette(palette);
    }
    this->updateRoutingPlatformButton();

    {
        QPalette palette = this->ui_.historySearchWrap->palette();
        palette.setColor(QPalette::Window, getTheme()->splits.input.background);
        if (!this->historySearchFailed)
        {
            palette.setColor(QPalette::Text, getTheme()->splits.input.text);
            palette.setColor(QPalette::WindowText,
                             getTheme()->splits.input.text);
        }
        this->ui_.historySearchWrap->setPalette(palette);
    }

    // Theme changed, reset current background color
    this->setBackgroundColor(this->theme->splits.input.background);
    this->backgroundColorAnimation.setStartValue(
        this->theme->splits.input.backgroundPulse);
    this->backgroundColorAnimation.setEndValue(
        this->theme->splits.input.background);
    this->backgroundColorAnimation.stop();
    this->updateTextEditPalette();

    if (this->theme->isLightTheme())
    {
        this->ui_.replyLabel->setStyleSheet("color: #333");
    }
    else
    {
        this->ui_.replyLabel->setStyleSheet("color: #ccc");
    }

    // update vbox
    this->applyOuterMargin();
    if (this->replyTarget_ != nullptr)
    {
        this->ui_.vbox->setSpacing(this->marginForTheme());
    }
}

void SplitInput::updateEmoteButton()
{
    auto scale = this->scale();

    this->ui_.emoteButton->setFixedHeight(int(18 * scale));
    // Make button slightly wider so it's easier to click
    this->ui_.emoteButton->setFixedWidth(int(24 * scale));
}

MessageDraft SplitInput::currentMessageDraft(QString text) const
{
    auto draft = this->draftTracker_.snapshot(std::move(text));
    draft.destinationPlatformOverride =
        this->effectiveRoutingPlatformOverride();
    return draft;
}

const std::optional<QString> &
SplitInput::effectiveRoutingPlatformOverride() const
{
    return this->draftRoutingOverrideActive_
               ? this->draftRoutingPlatformOverride_
               : this->routingPlatformOverride_;
}

void SplitInput::setRoutingPlatformOverride(std::optional<QString> platform)
{
    if (platform)
    {
        *platform = platform->trimmed().toLower();
        if (platform->isEmpty())
        {
            platform.reset();
        }
    }
    if (platform &&
        platform->compare(QStringLiteral("rumble"), Qt::CaseInsensitive) == 0)
    {
        auto channel = this->split_->getChannel();
        auto *multi = dynamic_cast<MultiChannel *>(channel.get());
        if (multi == nullptr ||
            !multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble))
        {
            platform.reset();
        }
    }
    const bool draftOverrideChanged = this->draftRoutingOverrideActive_;
    this->draftRoutingOverrideActive_ = false;
    this->draftRoutingPlatformOverride_.reset();
    if (platform == this->routingPlatformOverride_ && !draftOverrideChanged)
    {
        return;
    }
    this->routingPlatformOverride_ = std::move(platform);
    this->rebuildRoutingPlatformMenu();
    this->updateRoutingPlatformButton();
    this->updateCompletionPopup();
}

void SplitInput::rebuildRoutingPlatformMenu()
{
    auto channel = this->split_->getChannel();
    auto *multi = dynamic_cast<MultiChannel *>(channel.get());
    if (multi == nullptr)
    {
        return;
    }

    auto menu = std::make_unique<QMenu>();
    menu->setObjectName(QStringLiteral("multiChannelRoutingPlatformMenu"));
    menu->setAccessibleName(
        QStringLiteral("Choose multi-channel destination platform"));

    auto *automatic = menu->addAction(QStringLiteral("Automatic routing"));
    automatic->setCheckable(true);
    automatic->setChecked(!this->effectiveRoutingPlatformOverride());
    automatic->setToolTip(
        QStringLiteral("Let Chatterino choose the best destination"));
    QObject::connect(automatic, &QAction::triggered, this, [this] {
        this->setRoutingPlatformOverride(std::nullopt);
    });
    menu->addSeparator();

    std::set<MultiChannel::Platform> added;
    for (const auto &child : multi->channels())
    {
        if (!added.emplace(child.platform).second)
        {
            continue;
        }
        const auto id = multiChannelPlatformID(child.platform);
        const auto name = multiChannelPlatformName(child.platform);
        auto *action = menu->addAction(
            QIcon(platformBadgeResource(messagePlatform(child.platform), true)
                      .toString()),
            name);
        action->setCheckable(true);
        const auto available =
            multi->isRoutingPlatformAvailable(child.platform);
        action->setEnabled(available);
        action->setChecked(this->effectiveRoutingPlatformOverride() &&
                           this->effectiveRoutingPlatformOverride()->compare(
                               id, Qt::CaseInsensitive) == 0);
        action->setToolTip(
            available
                ? QStringLiteral("Route sends and emote completion only to %1")
                      .arg(name)
                : QStringLiteral(
                      "%1 routing is unavailable until a live chat session "
                      "is connected")
                      .arg(name));
        QObject::connect(action, &QAction::triggered, this, [this, id] {
            this->setRoutingPlatformOverride(id);
        });
    }

    this->ui_.routingPlatformButton->setMenu(std::move(menu));
}

void SplitInput::refreshRoutingPlatformAvailability()
{
    auto channel = this->split_->getChannel();
    auto *multi = dynamic_cast<MultiChannel *>(channel.get());
    const bool rumbleOverride =
        this->effectiveRoutingPlatformOverride() &&
        this->effectiveRoutingPlatformOverride()->compare(QStringLiteral("rumble"),
                                                Qt::CaseInsensitive) == 0;
    if (rumbleOverride &&
        (multi == nullptr ||
         !multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble)))
    {
        this->setRoutingPlatformOverride(std::nullopt);
        return;
    }
    this->rebuildRoutingPlatformMenu();
    this->updateRoutingPlatformButton();
}

bool SplitInput::cycleDraftRoutingOverride(bool reverse)
{
    auto channel = this->split_->getChannel();
    auto *multi = dynamic_cast<MultiChannel *>(channel.get());
    if (multi == nullptr || multi->channels().empty())
    {
        return false;
    }

    const auto text = this->ui_.textEdit->toPlainText();
    auto sendText = text;
    sendText.replace('\n', ' ');
    const auto primary = multi->activeChannelIndex();
    const auto policy = this->replyTarget_ != nullptr ||
                                isChannelBoundInput(text)
                            ? MultiChannelRoutePolicy::PrimaryOnly
                            : MultiChannelRoutePolicy::CompatibleFallback;

    std::vector<std::optional<QString>> choices;
    choices.emplace_back(std::nullopt);
    std::set<QString> seen;
    for (const auto &child : multi->channels())
    {
        if (!multi->isRoutingPlatformAvailable(child.platform))
        {
            continue;
        }
        const auto platform = multiChannelPlatformID(child.platform);
        if (!seen.emplace(platform).second)
        {
            continue;
        }
        auto draft = this->draftTracker_.snapshot(text);
        draft.destinationPlatformOverride = platform;
        if (multi->previewMessageDraftDestination(draft, sendText, primary,
                                                  policy)
                .sent())
        {
            choices.emplace_back(platform);
        }
    }

    if (choices.size() < 2)
    {
        return false;
    }

    size_t current = 0;
    if (this->draftRoutingOverrideActive_)
    {
        const auto it = std::ranges::find(
            choices, this->draftRoutingPlatformOverride_);
        if (it != choices.end())
        {
            current = static_cast<size_t>(std::distance(choices.begin(), it));
        }
    }

    const auto offset = reverse ? choices.size() - 1 : 1;
    const auto next = (current + offset) % choices.size();
    this->draftRoutingOverrideActive_ = true;
    this->draftRoutingPlatformOverride_ = choices[next];
    this->ui_.textEdit->setFocus(Qt::OtherFocusReason);
    this->updateRoutingPlatformButton();
    this->updateCompletionPopup();
    return true;
}

void SplitInput::updateRoutingPlatformButton()
{
    const auto hideAutomaticIndicator = [this] {
        this->ui_.automaticRoutingIndicator->hide();
        this->ui_.automaticRoutingIndicator->setToolTip({});
        this->ui_.automaticRoutingIndicator->setAccessibleName(
            QStringLiteral("Automatic routing indicator"));
    };

    auto channel = this->split_->getChannel();
    auto *multi = dynamic_cast<MultiChannel *>(channel.get());
    if (multi == nullptr || multi->channels().empty())
    {
        hideAutomaticIndicator();
        this->ui_.routingPlatformButton->hide();
        return;
    }

    hideAutomaticIndicator();
    this->ui_.routingPlatformButton->show();
    const auto text = this->ui_.textEdit->toPlainText();
    auto sendText = text;
    sendText.replace(u'\n', u' ');
    auto primary = multi->activeChannelIndex();
    if (this->replyTarget_ != nullptr)
    {
        if (const auto replyChannel = this->replyChannel_.lock())
        {
            const auto children = multi->channels();
            const auto replyChild = std::ranges::find(
                children, replyChannel, &MultiChannel::ChildChannel::channel);
            if (replyChild != children.end())
            {
                primary = static_cast<size_t>(
                    std::distance(children.begin(), replyChild));
            }
        }
    }
    const auto policy =
        this->replyTarget_ != nullptr || isChannelBoundInput(text)
            ? MultiChannelRoutePolicy::PrimaryOnly
            : MultiChannelRoutePolicy::CompatibleFallback;
    const auto preview = multi->previewMessageDraftDestination(
        this->currentMessageDraft(text), sendText, primary, policy);

    std::optional<size_t> visualIndex = preview.destinationIndex;
    if (!visualIndex && this->effectiveRoutingPlatformOverride())
    {
        for (size_t index = 0; index < multi->channels().size(); ++index)
        {
            const auto platform =
                multiChannelPlatformID(multi->channels()[index].platform);
            if (platform.compare(*this->effectiveRoutingPlatformOverride(),
                                 Qt::CaseInsensitive) == 0)
            {
                visualIndex = index;
                break;
            }
        }
    }
    if (!visualIndex)
    {
        this->ui_.routingTemporaryMarker->hide();
        this->ui_.routingPlatformButton->setPixmap({});
        this->ui_.routingPlatformButton->setBorderColor({});
        this->ui_.routingPlatformButton->setToolTip(
            QStringLiteral("No multi-channel destination is available"));
        this->ui_.routingPlatformButton->setAccessibleName(
            QStringLiteral("No multi-channel destination is available"));
        return;
    }

    const auto &child = multi->channels()[*visualIndex];
    const auto platformName = multiChannelPlatformName(child.platform);
    this->ui_.routingPlatformButton->setPixmap(
        QPixmap(platformBadgeResource(messagePlatform(child.platform), true)
                    .toString()));
    this->ui_.routingTemporaryMarker->setVisible(
        this->draftRoutingOverrideActive_);
    if (this->effectiveRoutingPlatformOverride())
    {
        this->ui_.routingPlatformButton->setBorderColor(
            this->theme->isLightTheme() ? QColor(QStringLiteral("#c62828"))
                                        : QColor(QStringLiteral("#ff5f56")));
        const auto availability =
            preview.destinationIndex
                ? QString{}
                : QStringLiteral(" (currently unavailable)");
        const auto scope = this->draftRoutingOverrideActive_
                               ? QStringLiteral("One-message routing override")
                               : QStringLiteral("Routing override");
        this->ui_.routingPlatformButton->setToolTip(
            QStringLiteral("%1: %2%3. Click to change or return to automatic "
                           "routing.")
                .arg(scope, platformName, availability));
        this->ui_.routingPlatformButton->setAccessibleName(
            QStringLiteral("%1: %2%3")
                .arg(scope, platformName, availability));
    }
    else
    {
        this->ui_.routingPlatformButton->setBorderColor({});
        this->ui_.routingPlatformButton->setToolTip(
            QStringLiteral("Automatic destination: %1. Click to override.")
                .arg(platformName));
        this->ui_.routingPlatformButton->setAccessibleName(
            QStringLiteral("Automatic multi-channel destination: %1")
                .arg(platformName));
        if (preview.usedFallback)
        {
            const auto explanation =
                QStringLiteral(
                    "Automatically routed to %1 — best available destination "
                    "for this message.")
                    .arg(platformName);
            this->ui_.automaticRoutingIndicator->setToolTip(explanation);
            this->ui_.automaticRoutingIndicator->setAccessibleName(
                explanation);
            this->ui_.automaticRoutingIndicator->show();
        }
    }
}

void SplitInput::updateCancelReplyButton()
{
    float scale = this->scale();

    this->ui_.cancelReplyButton->setFixedHeight(int(12 * scale));
    this->ui_.cancelReplyButton->setFixedWidth(int(20 * scale));
}

void SplitInput::openEmotePopup()
{
    if (!this->emotePopup_)
    {
        this->emotePopup_ = new EmotePopup(this);
        this->emotePopup_->setAttribute(Qt::WA_DeleteOnClose);

        // The EmotePopup is closed & destroyed when this is destroyed, meaning it's safe to ignore this connection
        std::ignore =
            this->emotePopup_->linkClicked.connect(
                [this](const Link &link,
                       const std::optional<DraftEmoteCandidate> &candidate,
                       bool provenanceAmbiguous) {
                this->insertEmotePopupSelection(link, candidate,
                                                provenanceAmbiguous);
            });
    }

    this->emotePopup_->loadChannel(this->split_->getSelectedChannel(),
                                   this->split_->getChannel());
    this->emotePopup_->show();
    this->emotePopup_->raise();
    this->emotePopup_->activateWindow();
}

void SplitInput::closeEmotePopup()
{
    if (this->emotePopup_)
    {
        this->emotePopup_->close();
    }
}

void SplitInput::insertEmotePopupSelection(
    const Link &link, const std::optional<DraftEmoteCandidate> &candidate,
    bool provenanceAmbiguous)
{
    if (link.type == Link::InsertText)
    {
        QTextCursor cursor = this->ui_.textEdit->textCursor();
        QString textToInsert(link.value + " ");
        auto candidateStart = cursor.selectionStart();

        // If symbol before cursor isn't space or empty
        // Then insert space before emote.
        if (candidateStart > 0 &&
            !this->getInputText()[candidateStart - 1].isSpace())
        {
            textToInsert = " " + textToInsert;
            ++candidateStart;
        }
        this->insertText(textToInsert);
        if (candidate)
        {
            this->draftTracker_.recordSelection(
                candidateStart, *candidate,
                this->ui_.textEdit->toPlainText());
        }
        else if (provenanceAmbiguous)
        {
            this->draftTracker_.recordUnresolvedSelection(
                candidateStart, link.value.size(),
                this->ui_.textEdit->toPlainText());
        }
        this->updateRoutingPlatformButton();
        this->ui_.textEdit->activateWindow();
        this->closeEmotePopup();
        this->giveFocus(Qt::OtherFocusReason);
    }
}

QString SplitInput::handleSendMessage(const std::vector<QString> &arguments)
{
    const auto aggregateChannel = this->split_->getChannel();
    auto *multi = dynamic_cast<MultiChannel *>(aggregateChannel.get());
    const size_t primaryAtSubmission =
        multi ? multi->activeChannelIndex() : 0;
    ChannelPtr c;
    if (this->replyTarget_)
    {
        c = this->replyChannel_.lock();
        if (!c)
        {
            const auto failure = QStringLiteral(
                "Reply not sent — the original destination is no longer "
                "available. Reselect the reply target and try again.");
            if (aggregateChannel)
            {
                aggregateChannel->addSystemMessage(failure);
            }
            return failure;
        }
        if (multi && std::ranges::none_of(
                         multi->channels(), [&c](const auto &child) {
                             return child.channel == c;
                         }))
        {
            const auto failure = QStringLiteral(
                "Reply not sent — its destination is no longer part of this "
                "multi-channel split.");
            aggregateChannel->addSystemMessage(failure);
            return failure;
        }
    }
    else
    {
        c = this->split_->getSelectedChannel();
    }
    if (c == nullptr)
    {
        return "";
    }

    if (this->replyTarget_ != nullptr && c->isRumbleChannel())
    {
        // Rumble cannot represent a structured reply. Its inline reply text is
        // already the complete user-visible message, so retire only the reply
        // relationship and dispatch that text once through the normal Rumble
        // message path.
        QString message = this->ui_.textEdit->toPlainText();
        message.replace('\n', ' ');
        QString sendMessage =
            getApp()->getCommands()->execCommand(message, c, false);

        if (sendMessage.isEmpty())
        {
            this->clearReplyTarget();
            this->postMessageSend(message, arguments);
            return "";
        }

        if (multi)
        {
            const bool channelBound = isChannelBoundInput(message);
            const bool transformed = sendMessage != message;
            auto draft = channelBound && transformed
                             ? MessageDraft::fromPlainText(sendMessage)
                             : this->currentMessageDraft(message);
            draft.destinationPlatformOverride =
                this->effectiveRoutingPlatformOverride();

            this->clearReplyTarget();

            if (this->effectiveRoutingPlatformOverride() &&
                c->messageSendContext().platform.compare(
                    *this->effectiveRoutingPlatformOverride(),
                    Qt::CaseInsensitive) != 0)
            {
                const auto failure =
                    QStringLiteral("Message not sent — the active routing "
                                   "override is set to %1. Change or clear "
                                   "the override to send here.")
                        .arg(*this->effectiveRoutingPlatformOverride());
                aggregateChannel->addSystemMessage(failure);
                return failure;
            }

            const auto evaluation = chatterino::evaluateMessageDraft(
                draft, c->messageSendContext(), sendMessage);
            if (!evaluation.sendable)
            {
                const auto failure = ordinaryDraftFailure(*c, evaluation);
                aggregateChannel->addSystemMessage(failure);
                return failure;
            }
        }
        else
        {
            this->clearReplyTarget();
        }

        const QPointer<SplitInput> guarded(this);
        const QPointer<QWidget> focusAtSubmission =
            QApplication::focusWidget();
        const std::weak_ptr<Channel> destination = c;
        const auto revision = this->draftTracker_.revision();
        c->sendMessageAsync(
            sendMessage, [guarded, destination, revision, message,
                          focusAtSubmission,
                          arguments](Channel::SendResult result) {
                if (!guarded)
                {
                    return;
                }
                if (result.outcome == Channel::SendOutcome::Confirmed)
                {
                    if (guarded->draftTracker_.revision() == revision)
                    {
                        guarded->postMessageSend(
                            message, arguments,
                            guarded->shouldRestoreFocus(focusAtSubmission));
                    }
                    return;
                }
                if (const auto channel = destination.lock();
                    channel && !result.userMessage.isEmpty())
                {
                    channel->addSystemMessage(result.userMessage);
                }
            });
        return "";
    }

    if (!c->isTwitchOrKickChannel() || this->replyTarget_ == nullptr)
    {
        // standard message send behavior
        QString message = this->ui_.textEdit->toPlainText();

        message = message.replace('\n', ' ');
        QString sendMessage =
            getApp()->getCommands()->execCommand(message, c, false);

        if (multi && this->replyTarget_ == nullptr)
        {
            // A local command may intentionally produce no provider message.
            // It is complete at this point; never call a child with an empty
            // payload.
            if (sendMessage.isEmpty())
            {
                this->postMessageSend(message, arguments);
                return "";
            }

            const bool channelBound = isChannelBoundInput(message);
            const bool transformed = sendMessage != message;
            // Commands are selected and expanded against the active child and
            // therefore stay primary-only. Ordinary emoji shortcode expansion
            // may change the provider payload, but routing still evaluates the
            // exact pre-expansion completion provenance.
            auto draft = channelBound && transformed
                             ? MessageDraft::fromPlainText(sendMessage)
                             : this->currentMessageDraft(message);
            draft.destinationPlatformOverride =
                this->effectiveRoutingPlatformOverride();
            const auto policy =
                channelBound ? MultiChannelRoutePolicy::PrimaryOnly
                             : MultiChannelRoutePolicy::CompatibleFallback;
            const QPointer<SplitInput> guarded(this);
            const QPointer<QWidget> focusAtSubmission =
                QApplication::focusWidget();
            const auto revision = this->draftTracker_.revision();
            const auto immediate = multi->sendMessageDraftAsync(
                draft, sendMessage, primaryAtSubmission, policy,
                [guarded, revision, message, focusAtSubmission, arguments](
                    MultiChannel::DraftDispatchResult result,
                    Channel::SendResult sendResult) mutable {
                    if (!guarded)
                        return;
                    if (!result.sent())
                        return;
                    if (sendResult.outcome == Channel::SendOutcome::Confirmed)
                    {
                        if (guarded->draftTracker_.revision() == revision)
                            guarded->postMessageSend(
                                message, arguments,
                                guarded->shouldRestoreFocus(focusAtSubmission));
                        return;
                    }
                    if (result.destination &&
                        !sendResult.userMessage.isEmpty())
                    {
                        result.destination->addSystemMessage(
                            sendResult.userMessage);
                    }
                });
            if (!immediate.sent())
            {
                const auto failure = draftRoutingFailure(
                    *multi, immediate,
                    policy == MultiChannelRoutePolicy::PrimaryOnly
                        ? std::optional<size_t>{primaryAtSubmission}
                        : std::nullopt);
                aggregateChannel->addSystemMessage(failure);
                return failure;
            }
            return "";
        }

        if (c->isRumbleChannel())
        {
            const QPointer<SplitInput> guarded(this);
            const QPointer<QWidget> focusAtSubmission =
                QApplication::focusWidget();
            const std::weak_ptr<Channel> destination = c;
            const auto revision = this->draftTracker_.revision();
            c->sendMessageAsync(
                sendMessage,
                [guarded, destination, revision, message,
                 focusAtSubmission, arguments](Channel::SendResult result) {
                    if (!guarded)
                        return;
                    if (result.outcome == Channel::SendOutcome::Confirmed)
                    {
                        // A completion may arrive after the operator has begun
                        // another draft. Clear only the exact submitted input.
                        if (guarded->draftTracker_.revision() == revision)
                        {
                            guarded->postMessageSend(
                                message, arguments,
                                guarded->shouldRestoreFocus(focusAtSubmission));
                        }
                        return;
                    }
                    if (const auto channel = destination.lock();
                        channel && !result.userMessage.isEmpty())
                    {
                        channel->addSystemMessage(result.userMessage);
                    }
                });
            return "";
        }

        c->sendMessage(sendMessage);

        this->postMessageSend(message, arguments);
        return "";
    }

    // Reply to message. Validate the live destination state before requiring a
    // concrete provider class so tests and alternate channel wrappers cannot
    // accidentally bypass the same rejection contract.
    const auto fullMessage = this->ui_.textEdit->toPlainText();
    QString message = fullMessage;
    qsizetype removedPrefix = 0;

    if (this->enableInlineReplying_)
    {
        // Remove @username prefix that is inserted when doing inline replies
        const auto mentionLength = this->replyTarget_->displayName.length() +
                                   1;  // remove "@username"
        removedPrefix += std::min<qsizetype>(mentionLength, message.size());
        message.remove(0, mentionLength);

        if (!message.isEmpty() && message.at(0) == ' ')
        {
            message.remove(0, 1);  // remove possible space
            ++removedPrefix;
        }
    }

    message = message.replace('\n', ' ');
    QString sendMessage =
        getApp()->getCommands()->execCommand(message, c, false);

    if (multi)
    {
        // A local command can complete without producing provider text. This is
        // not a reply send attempt and retains the existing command behavior.
        if (sendMessage.isEmpty())
        {
            this->postMessageSend(message, arguments);
            return "";
        }

        const bool channelBound = isChannelBoundInput(message);
        const bool transformed = sendMessage != message;
        auto draft = channelBound && transformed
                         ? MessageDraft::fromPlainText(sendMessage)
                         : rebaseDraftAfterPrefixRemoval(
                               this->currentMessageDraft(fullMessage),
                               removedPrefix, message);
        draft.destinationPlatformOverride =
            this->effectiveRoutingPlatformOverride();
        if (this->effectiveRoutingPlatformOverride() &&
            c->messageSendContext().platform.compare(
                *this->effectiveRoutingPlatformOverride(),
                Qt::CaseInsensitive) != 0)
        {
            const auto failure =
                QStringLiteral("Reply not sent — the active routing "
                               "override is set to %1. Change or clear "
                               "the override to reply here.")
                    .arg(*this->effectiveRoutingPlatformOverride());
            aggregateChannel->addSystemMessage(failure);
            return failure;
        }
        const auto evaluation = chatterino::evaluateMessageDraft(
            draft, c->messageSendContext(), sendMessage);
        if (!evaluation.sendable)
        {
            const auto failure = replyDraftFailure(*c, evaluation);
            aggregateChannel->addSystemMessage(failure);
            return failure;
        }
    }

    auto *tc = dynamic_cast<TwitchChannel *>(c.get());
    auto *kc = dynamic_cast<KickChannel *>(c.get());
    if (!tc && !kc)
    {
        // This should not fail for production Twitch/Kick channels. Keep the
        // input and reply binding intact when an alternate wrapper is used.
        return "";
    }

    // Reply within TwitchChannel
    if (tc)
    {
        tc->sendReply(sendMessage, this->replyTarget_->id);
    }
    else if (kc)
    {
        kc->sendReply(sendMessage, this->replyTarget_->id);
    }

    this->postMessageSend(message, arguments);
    return "";
}

void SplitInput::postMessageSend(const QString &message,
                                 const std::vector<QString> &arguments,
                                 bool restoreFocus)
{
    // don't add duplicate messages and empty message to message history
    if ((this->prevMsg_.isEmpty() || !this->prevMsg_.endsWith(message)) &&
        !message.trimmed().isEmpty())
    {
        this->prevMsg_.append(message);
    }

    if (arguments.empty() || arguments.at(0) != "keepInput")
    {
        this->clearInput();
    }
    this->prevIndex_ = this->prevMsg_.size();
    this->hideCompletionPopup();
    // A confirmed send has cleared the draft. Keep composition in this input
    // rather than leaving focus on a completion or send-control surface.
    if (restoreFocus)
    {
        const QPointer<SplitInput> guarded(this);
        QTimer::singleShot(0, this, [guarded] {
            if (!guarded)
            {
                return;
            }
            auto *focus = QApplication::focusWidget();
            if (focus == nullptr || guarded->isAncestorOf(focus))
            {
                guarded->giveFocus(Qt::OtherFocusReason);
            }
        });
    }
    this->draftRoutingPlatformOverride_.reset();
    this->draftRoutingOverrideActive_ = false;
    this->updateRoutingPlatformButton();
}

int SplitInput::scaledMaxHeight() const
{
    if (this->replyTarget_ != nullptr)
    {
        // give more space for showing the message being replied to
        return int(250 * this->scale());
    }
    else
    {
        return int(150 * this->scale());
    }
}

void SplitInput::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"cursorToStart",
         [this](const std::vector<QString> &arguments) -> QString {
             this->stopHistorySearchIfNecessary();

             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::Start;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart select argument (0)!";
                 return "Invalid cursorToStart select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"cursorToEnd",
         [this](const std::vector<QString> &arguments) -> QString {
             this->stopHistorySearchIfNecessary();

             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::End;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd select argument (0)!";
                 return "Invalid cursorToEnd select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"openEmotesPopup",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->openEmotePopup();
             return "";
         }},
        {"sendMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             this->stopHistorySearchIfNecessary();
             return this->handleSendMessage(arguments);
         }},
        {"previousMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;
             this->selectPreviousMessage();
             return "";
         }},
        {"nextMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;
             this->selectNextMessage();
             return "";
         }},
        {"undo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;
             this->stopHistorySearchIfNecessary();

             this->ui_.textEdit->undo();
             return "";
         }},
        {"redo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;
             this->stopHistorySearchIfNecessary();

             this->ui_.textEdit->redo();
             return "";
         }},
        {"copy",
         [this](const std::vector<QString> &arguments) -> QString {
             // XXX: this action is unused at the moment, a qt standard shortcut is used instead
             if (arguments.empty())
             {
                 return "copy action takes only one argument: the source "
                        "of the copy \"split\", \"input\" or "
                        "\"auto\". If the source is \"split\", only text "
                        "from the chat will be copied. If it is "
                        "\"splitInput\", text from the input box will be "
                        "copied. Automatic will pick whichever has a "
                        "selection";
             }

             bool copyFromSplit = false;
             const auto &mode = arguments.at(0);
             if (mode == "split")
             {
                 copyFromSplit = true;
             }
             else if (mode == "splitInput")
             {
                 copyFromSplit = false;
             }
             else if (mode == "auto")
             {
                 const auto &cursor = this->ui_.textEdit->textCursor();
                 copyFromSplit = !cursor.hasSelection();
             }

             if (copyFromSplit)
             {
                 this->channelView_->copySelectedText();
             }
             else
             {
                 this->ui_.textEdit->copy();
             }
             return "";
         }},
        {"paste",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;
             this->stopHistorySearchIfNecessary();

             this->ui_.textEdit->paste();
             return "";
         }},
        {"clear",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->clearInput();
             return "";
         }},
        {"selectAll",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->selectAll();
             return "";
         }},
        {"selectWord",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             auto cursor = this->ui_.textEdit->textCursor();
             cursor.select(QTextCursor::WordUnderCursor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"incremental-search-history",
         [this](const auto &args) -> QString {
             bool backwards = false;
             bool loop = false;
             if (args.size() >= 2)
             {
                 backwards = args[0] == u"backward"_s;
                 loop = args[1] == u"loop"_s;
             }
             this->startHistorySearch(backwards, loop);
             return {};
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::SplitInput, actions, this->parentWidget());
}

bool SplitInput::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == this->ui_.routingPlatformButton &&
        event->type() == QEvent::KeyPress)
    {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Enter ||
            keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Down)
        {
            if (auto *menu = this->ui_.routingPlatformButton->menu())
            {
                menu->popup(this->ui_.routingPlatformButton->mapToGlobal(
                    QPoint{0, this->ui_.routingPlatformButton->height()}));
                if (!menu->actions().empty())
                {
                    menu->setActiveAction(menu->actions().front());
                }
                event->accept();
                return true;
            }
        }
    }

    if (event->type() == QEvent::ShortcutOverride ||
        event->type() == QEvent::Shortcut)
    {
        if (auto *popup = this->inputCompletionPopup_.data())
        {
            if (popup->isVisible())
            {
                // Stop shortcut from triggering by saying we will handle it ourselves
                event->accept();

                // Return false means the underlying event isn't stopped, it will continue to propagate
                return false;
            }
        }
    }

    return BaseWidget::eventFilter(obj, event);
}

void SplitInput::installTextEditEvents()
{
    // We can safely ignore this signal's connection because SplitInput owns
    // the textEdit object, so it will always be deleted before SplitInput
    std::ignore =
        this->ui_.textEdit->keyPressed.connect([this](QKeyEvent *event) {
            if (auto *popup = this->inputCompletionPopup_.data())
            {
                if (popup->isVisible())
                {
                    if (popup->eventFilter(nullptr, event))
                    {
                        event->accept();
                        return;
                    }
                }
            }

            const bool forwardTab = event->key() == Qt::Key_Tab &&
                                    event->modifiers() == Qt::NoModifier;
            const bool backwardTab =
                (event->key() == Qt::Key_Backtab &&
                 (event->modifiers() == Qt::NoModifier ||
                  event->modifiers() == Qt::ShiftModifier)) ||
                (event->key() == Qt::Key_Tab &&
                 event->modifiers() == Qt::ShiftModifier);
            if ((forwardTab || backwardTab) &&
                this->cycleDraftRoutingOverride(backwardTab))
            {
                event->accept();
                return;
            }

            // One of the last remaining of it's kind, the copy shortcut.
            // For some bizarre reason Qt doesn't want this key be rebound.
            // TODO(Mm2PL): Revisit in Qt6, maybe something changed?
            if ((event->key() == Qt::Key_C || event->key() == Qt::Key_Insert) &&
                event->modifiers() == Qt::ControlModifier)
            {
                if (this->channelView_->hasSelection())
                {
                    this->channelView_->copySelectedText();
                    event->accept();
                }
            }
        });

    std::ignore = this->ui_.textEdit->contextMenuRequested.connect(
        [this](QMenu *menu, QPoint pos) {
            auto channel = this->split_->getChannel();
            if (auto *mc = dynamic_cast<MultiChannel *>(channel.get()))
            {
                auto channels = mc->channels();
                auto currentIdx = mc->activeChannelIndex();
                if (!channels.empty())
                {
                    auto *submenu = menu->addMenu("Set Context");
                    auto *group = new QActionGroup(submenu);

                    for (size_t i = 0; i < channels.size(); i++)
                    {
                        QString name =
                            multiChannelChildDisplayName(channels[i]) % u" (";
                        name +=
                            qmagicenum::enumNameString(channels[i].platform);
                        name += ')';
                        auto *action = new QAction(name, submenu);
                        action->setActionGroup(group);
                        action->setCheckable(true);
                        action->setChecked(i == currentIdx);
                        QObject::connect(
                            action, &QAction::toggled, this,
                            [this, i](bool checked) {
                                if (!checked)
                                {
                                    return;
                                }
                                auto *mc = dynamic_cast<MultiChannel *>(
                                    this->split_->getChannel().get());
                                mc->setActiveChannelIndex(i);
                                getApp()
                                    ->getWindows()
                                    ->forceLayoutChannelViews();
                            });
                        submenu->addAction(action);
                    }
                }
            }

#ifdef CHATTERINO_WITH_SPELLCHECK
            menu->addSeparator();
            auto *spellcheckAction = new QAction("Check spelling", menu);
            spellcheckAction->setCheckable(true);
            spellcheckAction->setChecked(this->shouldCheckSpelling());
            QObject::connect(spellcheckAction, &QAction::toggled, this,
                             [this](bool enabled) {
                                 this->checkSpellingOverride_ = enabled;
                                 this->checkSpellingChanged();
                             });
            menu->addAction(spellcheckAction);

            int nSuggestions = getSettings()->nSpellCheckingSuggestions;
            if (nSuggestions < 0)
            {
                nSuggestions = std::numeric_limits<int>::max();
            }

            if (!this->inputHighlighter || nSuggestions == 0)
            {
                return;
            }

            auto cursorAtPos = this->ui_.textEdit->cursorForPosition(pos);
            QString text = this->ui_.textEdit->toPlainText();
            QStringView word =
                this->inputHighlighter->getWordAt(text, cursorAtPos.position());
            if (!word.isEmpty())
            {
                auto cursor = this->ui_.textEdit->textCursor();
                // Select `word`. `word` is a view into `text`, so we can use
                // the offsets of `word` from the start of `text`.
                cursor.setPosition(
                    static_cast<int>(word.begin() - text.begin()));
                cursor.setPosition(static_cast<int>(word.end() - text.begin()),
                                   QTextCursor::KeepAnchor);

                auto suggestions =
                    getApp()->getSpellChecker()->suggestions(word.toString());
                for (const auto &sugg :
                     suggestions | std::views::take(nSuggestions))
                {
                    auto qSugg = QString::fromStdString(sugg);
                    menu->addAction(qSugg, [this, qSugg, cursor]() mutable {
                        cursor.insertText(qSugg);
                        this->ui_.textEdit->setTextCursor(cursor);
                    });
                }
            }
#else
            (void)menu;
            (void)pos;
            (void)this;
#endif
        });
}

void SplitInput::mousePressEvent(QMouseEvent *event)
{
    this->giveFocus(Qt::MouseFocusReason);

    if (this->hidden)
    {
        BaseWidget::mousePressEvent(event);
    }
    // else, don't call QWidget::mousePressEvent,
    // which will call event->ignore()
}

void SplitInput::onTextChanged()
{
    this->updateCompletionPopup();
}

void SplitInput::onCursorPositionChanged()
{
    this->updateCompletionPopup();
}

void SplitInput::updateCompletionPopup()
{
    auto *channel = this->split_->getSelectedChannel().get();
    auto *tc = dynamic_cast<TwitchChannel *>(channel);
    bool showEmoteCompletion = getSettings()->emoteCompletionWithColon;
    bool showUsernameCompletion =
        tc != nullptr && getSettings()->showUsernameCompletionMenu;
    if (!showEmoteCompletion && !showUsernameCompletion)
    {
        this->hideCompletionPopup();
        return;
    }

    // check if in completion prefix
    auto &edit = *this->ui_.textEdit;

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    if (text.length() == 0 || position == -1)
    {
        this->hideCompletionPopup();
        return;
    }

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        if (text[i] == ' ')
        {
            this->hideCompletionPopup();
            return;
        }

        if (text[i] == ':' && showEmoteCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::Emote);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }

        if (text[i] == '@' && showUsernameCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::User);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }
    }

    this->hideCompletionPopup();
}

void SplitInput::showCompletionPopup(const QString &text, CompletionKind kind)
{
    if (this->inputCompletionPopup_.isNull())
    {
        this->inputCompletionPopup_ = new InputCompletionPopup(this);
        this->inputCompletionPopup_->setInputAction(
            [that = QPointer(this)](
                const QString &text,
                const std::optional<DraftEmoteCandidate> &candidate,
                bool provenanceAmbiguous) mutable {
                if (auto *this2 = that.data())
                {
                    this2->insertCompletionText(text, candidate,
                                                provenanceAmbiguous);
                    this2->hideCompletionPopup();
                }
            });
    }

    auto *popup = this->inputCompletionPopup_.data();
    assert(popup);

    const auto &edit = *this->ui_.textEdit;
    auto draftText = edit.toPlainText();
    const auto queryEnd = edit.textCursor().position();
    const auto queryStart = queryEnd - text.size();
    auto draftTracker = this->draftTracker_;
    if (queryStart >= 0 && queryEnd <= draftText.size())
    {
        draftText.remove(queryStart, text.size());
        draftTracker.contentsChanged(queryStart, text.size(), 0, draftText);
    }
    auto draft = draftTracker.snapshot(std::move(draftText));
    draft.destinationPlatformOverride =
        this->effectiveRoutingPlatformOverride();

    auto completionChannel = kind == CompletionKind::Emote
                                 ? this->split_->getChannel()
                                 : this->split_->getSelectedChannel();
    popup->updateCompletion(text, kind, std::move(completionChannel),
                            draft);

    auto pos = this->mapToGlobal(QPoint{0, 0}) - QPoint(0, popup->height()) +
               QPoint((this->width() - popup->width()) / 2, 0);

    popup->move(pos);
    popup->show();
}

void SplitInput::hideCompletionPopup()
{
    if (auto *popup = this->inputCompletionPopup_.data())
    {
        popup->hide();
    }
}

void SplitInput::insertCompletionText(
    const QString &input_,
    const std::optional<DraftEmoteCandidate> &candidate,
    bool provenanceAmbiguous)
{
    auto &edit = *this->ui_.textEdit;
    auto input = input_ + ' ';

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        bool done = false;
        if (text[i] == ':')
        {
            done = true;
        }
        else if (text[i] == '@')
        {
            const auto userMention =
                formatUserMention(input_, edit.isFirstWord(),
                                  getSettings()->mentionUsersWithComma);
            input = "@" + userMention + " ";
            done = true;
        }

        if (done)
        {
            auto cursor = edit.textCursor();
            cursor.setPosition(i);
            cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
            cursor.insertText(input);
            edit.setTextCursor(cursor);
            if (candidate)
            {
                this->draftTracker_.recordSelection(
                    i, *candidate, edit.toPlainText());
            }
            else if (provenanceAmbiguous)
            {
                this->draftTracker_.recordUnresolvedSelection(
                    i, input_.size(), edit.toPlainText());
            }
            this->updateRoutingPlatformButton();
            break;
        }
    }
}

bool SplitInput::hasSelection() const
{
    return this->ui_.textEdit->textCursor().hasSelection();
}

void SplitInput::clearSelection() const
{
    auto cursor = this->ui_.textEdit->textCursor();
    cursor.clearSelection();
    this->ui_.textEdit->setTextCursor(cursor);
}

bool SplitInput::isEditFirstWord() const
{
    return this->ui_.textEdit->isFirstWord();
}

QString SplitInput::getInputText() const
{
    return this->ui_.textEdit->toPlainText();
}

void SplitInput::insertText(const QString &text)
{
    this->ui_.textEdit->insertPlainText(text);
}

void SplitInput::hide()
{
    if (this->isHidden())
    {
        return;
    }

    this->hidden = true;
    this->setMaximumHeight(0);
    this->updateGeometry();
}

void SplitInput::show()
{
    if (!this->isHidden())
    {
        return;
    }

    this->hidden = false;
    this->setMaximumHeight(this->scaledMaxHeight());
    this->updateGeometry();
}

bool SplitInput::isHidden() const
{
    return this->hidden;
}

bool SplitInput::isInHistorySearch() const
{
    return this->inHistorySearch;
}

void SplitInput::setInputText(const QString &newInputText)
{
    // setPlainText can report a remove/add range that includes the implicit
    // final paragraph separator. Clear on both sides so a replacement that is
    // explicitly meant to discard provenance cannot poison the fresh draft.
    this->draftTracker_.clear();
    this->ui_.textEdit->setPlainText(newInputText);
    this->draftTracker_.clear();
    this->trackedDocumentText_ = this->ui_.textEdit->toPlainText();
    this->trackedDocumentRichText_ =
        this->ui_.textEdit->document()->toHtml();
    this->updateRoutingPlatformButton();
}

void SplitInput::editTextChanged()
{
    auto *app = getApp();

    // set textLengthLabel value
    QString text = this->ui_.textEdit->toPlainText();

    if (this->shouldPreventInput(text))
    {
        const auto truncated = truncateDraft(
            this->draftTracker_.snapshot(text), TWITCH_MESSAGE_LIMIT);
        this->ui_.textEdit->setPlainText(truncated.text);
        restoreDraftTracking(this->draftTracker_, truncated);
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        return;
    }

    if (text.startsWith("/r ", Qt::CaseInsensitive) &&
        this->split_->getSelectedChannel()->isTwitchChannel())
    {
        auto lastUser = app->getTwitch()->getLastUserThatWhisperedMe();
        if (!lastUser.isEmpty())
        {
            this->ui_.textEdit->setPlainText("/w " + lastUser + text.mid(2));
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        }
    }
    else
    {
        this->textChanged.invoke(text);

        text = text.trimmed();
        text = app->getCommands()->execCommand(text, this->split_->getChannel(),
                                               true);
    }

    QList<QTextEdit::ExtraSelection> selections;
    if (text.length() > 0 &&
        getSettings()->messageOverflow.getValue() == MessageOverflow::Highlight)
    {
        QTextCursor cursor = this->ui_.textEdit->textCursor();
        QTextCharFormat format;

        cursor.setPosition(qMin(text.length(), TWITCH_MESSAGE_LIMIT),
                           QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        selections.append({cursor, format});

        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            cursor.setPosition(TWITCH_MESSAGE_LIMIT, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            format.setForeground(Qt::red);
            selections.append({cursor, format});
        }
    }

    if (!text.isEmpty() && this->inHistorySearch &&
        !this->historySearchFailed && !this->historySearchQuery.isEmpty())
    {
        auto matchIdx =
            text.indexOf(this->historySearchQuery, 0, Qt::CaseInsensitive);
        if (matchIdx >= 0)
        {
            QTextCursor cursor = this->ui_.textEdit->textCursor();
            QTextCharFormat format;
            format.setBackground(
                getTheme()->splits.input.searchHighlightBackground);
            format.setUnderlineStyle(QTextCharFormat::SingleUnderline);

            cursor.setPosition(static_cast<int>(matchIdx),
                               QTextCursor::MoveAnchor);
            cursor.setPosition(
                static_cast<int>(matchIdx + this->historySearchQuery.size()),
                QTextCursor::KeepAnchor);
            selections.append({.cursor = cursor, .format = format});
        }
    }

    // block reemit of QTextEdit::textChanged()
    {
        const QSignalBlocker b(this->ui_.textEdit);
        this->ui_.textEdit->setExtraSelections(selections);
    }

    QString labelText;

    if (text.length() > 0 && getSettings()->showMessageLength)
    {
        labelText = QString::number(text.length());
        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            this->ui_.textEditLength->setStyleSheet("color: red");
        }
        else
        {
            this->ui_.textEditLength->setStyleSheet("");
        }
    }
    else
    {
        labelText = "";
    }

    this->ui_.textEditLength->setText(labelText);

    bool hasReply = false;
    if (this->enableInlineReplying_)
    {
        if (this->replyTarget_ != nullptr)
        {
            // Check if the input still starts with @username. If not, don't reply.
            //
            // We need to verify that
            // 1. the @username prefix exists and
            // 2. if a character exists after the @username, it is a space
            QString replyPrefix = "@" + this->replyTarget_->displayName;
            if (!text.startsWith(replyPrefix) ||
                (text.length() > replyPrefix.length() &&
                 text.at(replyPrefix.length()) != ' '))
            {
                this->clearReplyTarget();
            }
        }

        // Show/hide reply label if inline replies are possible
        hasReply = this->replyTarget_ != nullptr;
    }

    this->ui_.replyWrapper->setVisible(hasReply);
    this->ui_.replyLabel->setVisible(hasReply);
    this->ui_.cancelReplyButton->setVisible(hasReply);
    this->updateRoutingPlatformButton();
}

void SplitInput::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor borderColor =
        this->theme->isLightTheme() ? QColor("#ccc") : QColor("#333");

    QRect baseRect = this->rect();
    baseRect.setWidth(baseRect.width() - 1);

    auto *inputWrap = this->ui_.inputWrapper;
    auto inputBoxRect = inputWrap->geometry();
    inputBoxRect.setSize(inputBoxRect.size() - QSize{1, 1});

    painter.setBrush({this->theme->splits.input.background});
    painter.setPen(borderColor);
    painter.drawRect(inputBoxRect);

    if (this->enableInlineReplying_ && this->replyTarget_ != nullptr)
    {
        auto replyRect = this->ui_.replyWrapper->geometry();
        replyRect.setSize(replyRect.size() - QSize{1, 1});

        painter.setBrush(this->theme->splits.input.background);
        painter.setPen(borderColor);
        painter.drawRect(replyRect);

        QPoint replyLabelBorderStart(
            replyRect.x(),
            replyRect.y() + this->ui_.replyHbox->geometry().height());
        QPoint replyLabelBorderEnd(replyRect.right(),
                                   replyLabelBorderStart.y());
        painter.drawLine(replyLabelBorderStart, replyLabelBorderEnd);
    }
}

void SplitInput::resizeEvent(QResizeEvent *event)
{
    (void)event;

    if (this->height() == this->maximumHeight())
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
    else
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    this->ui_.replyMessage->setWidth(this->replyMessageWidth());
}

void SplitInput::giveFocus(Qt::FocusReason reason)
{
    this->ui_.textEdit->setFocus(reason);
}

bool SplitInput::shouldRestoreFocus(
    const QPointer<QWidget> &focusAtSubmission) const
{
    return QApplication::focusWidget() == focusAtSubmission;
}

void SplitInput::setReply(MessagePtr target, std::weak_ptr<Channel> channel)
{
    this->resetReplyHistoryNavigation();

    const auto originalText = this->ui_.textEdit->toPlainText();
    const auto priorDraft = this->draftTracker_.snapshot(originalText);
    RetainedDraftSlice retained(originalText);
    qsizetype insertedPrefixLength = 0;
    auto oldParent = this->replyTarget_;
    if (this->enableInlineReplying_ && oldParent)
    {
        // Remove old reply prefix
        auto replyPrefix = "@" + oldParent->displayName;
        retained.trim();
        if (retained.text.startsWith(replyPrefix))
        {
            retained.removePrefix(replyPrefix.length());
        }
        retained.trim();
        this->ui_.textEdit->setPlainText(retained.text);
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        this->ui_.textEdit->resetCompletion();
    }

    if (target != nullptr)
    {
        this->replyTarget_ = std::move(target);
        this->replyChannel_ = std::move(channel);

        if (this->enableInlineReplying_)
        {
            this->ui_.replyMessage->setWidth(this->replyMessageWidth());
            this->ui_.replyMessage->setMessage(this->replyTarget_);

            // add spacing between reply box and input box
            this->ui_.vbox->setSpacing(this->marginForTheme());
            if (!this->isHidden())
            {
                // update maximum height to give space for message
                this->setMaximumHeight(this->scaledMaxHeight());
            }

            // Only enable reply label if inline replying
            auto replyPrefix = "@" + this->replyTarget_->displayName;
            retained.trim();

            // This makes it so if plainText contains "@StreamerFan" and
            // we are replying to "@Streamer" we don't just leave "Fan"
            // in the text box
            if (retained.text.startsWith(replyPrefix))
            {
                if (retained.text.length() > replyPrefix.length())
                {
                    if (retained.text.at(replyPrefix.length()) == ',' ||
                        retained.text.at(replyPrefix.length()) == ' ')
                    {
                        retained.removePrefix(replyPrefix.length() + 1);
                    }
                }
                else
                {
                    retained.removePrefix(replyPrefix.length());
                }
            }
            if (!retained.text.isEmpty() && !retained.text.startsWith(' '))
            {
                replyPrefix.append(' ');
            }
            insertedPrefixLength = replyPrefix.size();
            this->ui_.textEdit->setPlainText(replyPrefix + retained.text + " ");
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
            this->ui_.textEdit->resetCompletion();
            this->ui_.replyLabel->setText("Replying to @" +
                                          this->replyTarget_->displayName);
        }
    }
    else
    {
        this->replyTarget_.reset();
        this->replyChannel_.reset();

        if (this->enableInlineReplying_)
        {
            this->clearReplyTarget();
        }
    }

    // setPlainText can transiently poison even an unannotated draft because
    // Qt reports its implicit paragraph separator. Always restore the rebased
    // snapshot, including the valid plain-draft case.
    const auto text = this->ui_.textEdit->toPlainText();
    restoreDraftTracking(
        this->draftTracker_,
        rebaseDraftToRetainedSlice(priorDraft, retained,
                                   insertedPrefixLength, text));
}

void SplitInput::setPlaceholderText(const QString &text)
{
    this->ui_.textEdit->setPlaceholderText(text);
}

void SplitInput::clearInput()
{
    this->currMsg_ = "";
    this->stopHistorySearchIfNecessary();
    this->setInputText({});
    this->ui_.textEdit->moveCursor(QTextCursor::Start);
    if (this->enableInlineReplying_)
    {
        this->clearReplyTarget();
    }
}

void SplitInput::clearReplyTarget()
{
    this->replyTarget_.reset();
    this->replyChannel_.reset();
    this->resetReplyHistoryNavigation();
    this->ui_.replyMessage->clearMessage();
    this->ui_.replyWrapper->hide();
    this->ui_.replyLabel->hide();
    this->ui_.cancelReplyButton->hide();
    this->ui_.vbox->setSpacing(0);
    if (!this->isHidden())
    {
        this->setMaximumHeight(this->scaledMaxHeight());
    }
    this->updateRoutingPlatformButton();
}

void SplitInput::selectPreviousMessage()
{
    this->stopHistorySearchIfNecessary();

    if (this->prevMsg_.isEmpty() || this->prevIndex_ == 0)
    {
        return;
    }

    if (this->prevIndex_ == this->prevMsg_.size())
    {
        this->currMsg_ = this->ui_.textEdit->toPlainText();
        this->historyReplyTarget_ = this->replyTarget_;
    }

    --this->prevIndex_;
    const auto &message = this->prevMsg_.at(this->prevIndex_);
    const bool preserveReply = this->historyReplyTarget_ != nullptr &&
                               this->historyReplyTarget_ == this->replyTarget_;
    this->setInputText(
        preserveReply ? this->formatHistoryMessageForReply(message) : message);
    this->ui_.textEdit->resetCompletion();

    auto cursor = this->ui_.textEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    this->ui_.textEdit->setTextCursor(cursor);
}

void SplitInput::selectNextMessage()
{
    this->stopHistorySearchIfNecessary();

    if (this->prevMsg_.isEmpty())
    {
        return;
    }

    const bool preserveReply =
        this->historyReplyTarget_ != nullptr &&
        this->historyReplyTarget_ == this->replyTarget_ &&
        this->prevIndex_ < this->prevMsg_.size();
    if (preserveReply)
    {
        if (this->prevIndex_ < this->prevMsg_.size() - 1)
        {
            ++this->prevIndex_;
            this->setInputText(this->formatHistoryMessageForReply(
                this->prevMsg_.at(this->prevIndex_)));
        }
        else
        {
            this->prevIndex_ = this->prevMsg_.size();
            this->setInputText(this->currMsg_);
            this->historyReplyTarget_.reset();
        }
        this->ui_.textEdit->resetCompletion();

        auto cursor = this->ui_.textEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        this->ui_.textEdit->setTextCursor(cursor);
        return;
    }

    // Preserve the existing non-reply history behavior byte-for-byte.
    bool cursorToEnd = true;
    const auto message = this->ui_.textEdit->toPlainText();

    if (this->prevIndex_ != this->prevMsg_.size() - 1 &&
        this->prevIndex_ != this->prevMsg_.size())
    {
        ++this->prevIndex_;
        this->setInputText(this->prevMsg_.at(this->prevIndex_));
        this->ui_.textEdit->resetCompletion();
    }
    else
    {
        this->prevIndex_ = this->prevMsg_.size();
        if (message == this->prevMsg_.at(this->prevIndex_ - 1))
        {
            this->setInputText(this->currMsg_);
            this->ui_.textEdit->resetCompletion();
        }
        else if (message != this->currMsg_)
        {
            this->currMsg_ = message;
        }
        cursorToEnd = message == this->prevMsg_.at(this->prevIndex_ - 1);
    }

    if (cursorToEnd)
    {
        auto cursor = this->ui_.textEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        this->ui_.textEdit->setTextCursor(cursor);
    }
}

QString SplitInput::formatHistoryMessageForReply(const QString &message) const
{
    if (this->replyTarget_ == nullptr)
    {
        return message;
    }

    const auto prefix = QStringLiteral("@") + this->replyTarget_->displayName;
    const auto body = stripBoundaryReplyTags(message);
    if (body.isEmpty())
    {
        return prefix + QStringLiteral(" ");
    }
    return prefix + QStringLiteral(" ") + body;
}

void SplitInput::resetReplyHistoryNavigation()
{
    this->historyReplyTarget_.reset();
    this->prevIndex_ = this->prevMsg_.size();
}

bool SplitInput::shouldPreventInput(const QString &text) const
{
    if (getSettings()->messageOverflow.getValue() != MessageOverflow::Prevent)
    {
        return false;
    }

    auto channel = this->split_->getSelectedChannel();

    if (channel == nullptr)
    {
        return false;
    }

    if (!channel->isTwitchChannel())
    {
        // Don't respect this setting for IRC channels as the limits might be server-specific
        return false;
    }

    return text.length() > TWITCH_MESSAGE_LIMIT;
}

int SplitInput::marginForTheme() const
{
    if (this->theme->isLightTheme())
    {
        return int(3 * this->scale());
    }
    else
    {
        return int(1 * this->scale());
    }
}

void SplitInput::applyOuterMargin()
{
    auto margin = std::max(this->marginForTheme() - 1, 0);
    this->ui_.vbox->setContentsMargins(margin, margin, margin, margin);
}

int SplitInput::replyMessageWidth() const
{
    return this->ui_.inputWrapper->width() - 1 - 10;
}

void SplitInput::updateTextEditPalette()
{
    QPalette p;

    // Placeholder text color
    p.setColor(QPalette::PlaceholderText,
               this->theme->messages.textColors.chatPlaceholder);

    // Text color
    p.setColor(QPalette::Text, this->theme->messages.textColors.regular);

    // Selection background color
    p.setBrush(QPalette::Highlight,
               this->theme->isLightTheme()
                   ? QColor(u"#68B1FF"_s)
                   : this->theme->tabs.selected.backgrounds.regular);

    // Background color
    p.setBrush(QPalette::Base, this->backgroundColor());

    this->ui_.textEdit->setPalette(p);
}

QColor SplitInput::backgroundColor() const
{
    return this->backgroundColor_;
}

void SplitInput::setBackgroundColor(QColor newColor)
{
    this->backgroundColor_ = newColor;

    this->updateTextEditPalette();
}

std::optional<bool> SplitInput::checkSpellingOverride() const
{
    return this->checkSpellingOverride_;
}

void SplitInput::setCheckSpellingOverride(std::optional<bool> override)
{
    this->checkSpellingOverride_ = override;
    this->checkSpellingChanged();
}

bool SplitInput::shouldCheckSpelling() const
{
    if (this->checkSpellingOverride_)
    {
        return *this->checkSpellingOverride_;
    }
    return getSettings()->enableSpellChecking;
}

void SplitInput::checkSpellingChanged()
{
    QTextDocument *target = nullptr;
    if (this->shouldCheckSpelling())
    {
        target = this->ui_.textEdit->document();
    }

    if (this->inputHighlighter->document() != target)
    {
        this->inputHighlighter->setDocument(target);
    }
}

void SplitInput::updateFonts()
{
    auto *app = getApp();
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    // NOTE: We're using TimestampMedium here to get a font that uses the tnum font feature,
    // meaning numbers get equal width & don't bounce around while the user is typing.
    auto tsMedium =
        app->getFonts()->getFont(FontStyle::TimestampMedium, this->scale());
    this->ui_.textEditLength->setFont(tsMedium);
    this->ui_.sendWaitStatus->setFont(tsMedium);
    this->ui_.replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale()));

    this->ui_.historySearchWrap->setFont(getApp()->getFonts()->getFont(
        FontStyle::ChatMediumSmall, this->scale()));
}

void SplitInput::stopHistorySearchIfNecessary()
{
    if (!this->inHistorySearch || isAppAboutToQuit())
    {
        return;
    }
    this->inHistorySearch = false;
    this->historySearchStateChanged.invoke();
    this->ui_.historySearchWrap->hide();
    this->split_->setFocusProxy(this->ui_.textEdit);
    this->ui_.textEdit->setFocus();
    this->ui_.textEdit->moveCursor(QTextCursor::End);
    this->editTextChanged();
}

void SplitInput::startHistorySearch(bool backwards, bool loop)
{
    this->lastHistorySearchBackwards = backwards;
    this->lastHistorySearchLoop = loop;
    if (this->inHistorySearch)
    {
        this->cycleHistorySearch(backwards, loop);
        return;
    }
    this->ui_.historySearchInput->clear();
    this->ui_.historySearchWrap->setVisible(true);
    this->split_->setFocusProxy(this->ui_.historySearchInput);
    this->ui_.historySearchInput->setFocus(Qt::MouseFocusReason);
    this->historySearchQuery = {};
    this->inHistorySearch = true;
    this->historySearchStateChanged.invoke();
    this->prevIndexBeforeSearch = this->prevIndex_;
    this->refreshHistorySearch(backwards, loop);
}

void SplitInput::refreshHistorySearch(bool backwards, bool loop)
{
    if (!this->inHistorySearch)
    {
        return;
    }
    this->historySearchResults.clear();
    if (this->historySearchQuery.isEmpty())
    {
        this->ui_.historySearchLabel->clear();
        this->editTextChanged();
        return;
    }
    // `prevIndex_` might've changed because the user cycled through the results.
    // However, the initial position should be used as the anchor.
    this->prevIndex_ = this->prevIndexBeforeSearch;

    qsizetype closestMatch = -1;  // initial result
    for (qsizetype i = 0; i < this->prevMsg_.size(); i++)
    {
        auto message = this->prevMsg_.at(i);
        auto matchIdx =
            message.indexOf(this->historySearchQuery, 0, Qt::CaseInsensitive);
        if (matchIdx >= 0)
        {
            this->historySearchResults.emplace_back(
                HistorySearchResult{.messageIdx = i, .message = message});
        }

        if (i == this->prevIndex_)
        {
            closestMatch =
                static_cast<qsizetype>(this->historySearchResults.size()) - 1;
        }
    }
    if (this->prevIndex_ >= this->prevMsg_.size())
    {
        closestMatch =
            static_cast<qsizetype>(this->historySearchResults.size()) - 1;
    }

    // `closestMatch` points at the last message that's before or at `prevIndex_`.
    // For forwards search, we want it to be the first message not before `prevIndex_`.
    if (!backwards && closestMatch >= 0 &&
        static_cast<size_t>(closestMatch) < this->historySearchResults.size() &&
        this->historySearchResults[closestMatch].messageIdx != this->prevIndex_)
    {
        closestMatch++;
    }

    this->historySearchResultIndex = closestMatch;

    if (loop)
    {
        this->loopHistorySearchIfNeeded(backwards);
    }

    this->updateSelectedHistorySearchMatch();
}

void SplitInput::cycleHistorySearch(bool backwards, bool loop)
{
    if (backwards)
    {
        this->historySearchResultIndex--;
    }
    else
    {
        this->historySearchResultIndex++;
    }

    if (loop)
    {
        this->loopHistorySearchIfNeeded(backwards);
    }

    this->historySearchResultIndex =
        std::clamp(this->historySearchResultIndex, -1LL,
                   static_cast<qsizetype>(this->historySearchResults.size()));

    this->updateSelectedHistorySearchMatch();
}

void SplitInput::loopHistorySearchIfNeeded(bool backwards)
{
    if (backwards && this->historySearchResultIndex < 0)
    {
        this->historySearchResultIndex =
            static_cast<qsizetype>(this->historySearchResults.size()) - 1;
    }
    else if (!backwards &&
             std::cmp_greater_equal(this->historySearchResultIndex,
                                    this->historySearchResults.size()))
    {
        this->historySearchResultIndex = 0;
    }
}

void SplitInput::updateSelectedHistorySearchMatch()
{
    if (this->historySearchResultIndex < 0 ||
        std::cmp_greater_equal(this->historySearchResultIndex,
                               this->historySearchResults.size()))
    {
        this->updateHistorySearchStatus(true, "no match");
        return;
    }

    const auto &current = this->historySearchResults[static_cast<size_t>(
        this->historySearchResultIndex)];

    this->prevIndex_ = static_cast<int>(current.messageIdx);
    this->setInputText(current.message);

    this->updateHistorySearchStatus(
        false, QString::number(this->historySearchResults.size() -
                               this->historySearchResultIndex) %
                   '/' % QString::number(this->historySearchResults.size()));

    this->editTextChanged();
}

void SplitInput::updateHistorySearchStatus(bool failed, const QString &message)
{
    if (failed && !this->historySearchFailed)
    {
        QPalette palette = this->ui_.historySearchWrap->palette();
        auto failColor = getTheme()->splits.input.searchFailText;
        palette.setColor(QPalette::Text, failColor);
        palette.setColor(QPalette::WindowText, failColor);
        this->ui_.historySearchWrap->setPalette(palette);
    }
    else if (!failed && this->historySearchFailed)
    {
        QPalette palette = this->ui_.historySearchWrap->palette();
        palette.setColor(QPalette::Text, getTheme()->splits.input.text);
        palette.setColor(QPalette::WindowText, getTheme()->splits.input.text);
        this->ui_.historySearchWrap->setPalette(palette);
    }
    this->historySearchFailed = failed;

    this->ui_.historySearchLabel->setText(message);
}

void SplitInput::setSendWaitStatus(const QString &text) const
{
    this->ui_.sendWaitStatus->setText(text);
    if (text.isEmpty())
    {
        this->ui_.sendWaitStatus->setHidden(true);
    }
    else
    {
        this->ui_.sendWaitStatus->setHidden(!getSettings()->showSendWaitTimer);
    }
}

void SplitInput::updateChannel()
{
    this->channelConnections_.clear();
    this->routingPlatformOverride_.reset();
    this->draftRoutingPlatformOverride_.reset();
    this->draftRoutingOverrideActive_ = false;

    auto channel = this->split_->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            multiChannel->activeChannelChanged, [this] {
                auto selected = this->split_->getSelectedChannel();
                this->inputHighlighter->setChannel(selected);
                this->updateRoutingPlatformButton();
            });
        this->channelConnections_.managedConnect(
            multiChannel->childStateChanged, [this] {
                this->refreshRoutingPlatformAvailability();
                this->updateCompletionPopup();
            });
        this->ui_.textEdit->setCompleter(
            new QCompleter(channel->completionModel));
    }
    else
    {
        auto selected = this->split_->getSelectedChannel();
        this->ui_.textEdit->setCompleter(
            new QCompleter(selected->completionModel));
    }

    auto selected = this->split_->getSelectedChannel();
    this->inputHighlighter->setChannel(selected);
    this->rebuildRoutingPlatformMenu();
    this->updateRoutingPlatformButton();
}

}  // namespace chatterino

#include "SplitInput.moc"
