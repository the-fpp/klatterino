// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/WindowDescriptors.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/history/MessageHistoryLoadRegistry.hpp"
#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleApplicationController.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleLayoutLocator.hpp"
#include "providers/rumble/RumbleScheduler.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/helper/MicroNotebook.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryFile>
#include <QVariant>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace chatterino;
using namespace chatterino::rumble;

namespace {

class QueueDispatcher final : public RumbleDispatcher
{
public:
    bool isOwnerThread() const noexcept override
    {
        return this->owner;
    }

    bool dispatch(Task task) override
    {
        if (!this->accept)
        {
            return false;
        }
        std::lock_guard lock(this->mutex);
        this->tasks.push_back(std::move(task));
        return true;
    }

    void dispose(Task cleanup) noexcept override
    {
        if (this->owner)
        {
            cleanup();
            return;
        }
        std::lock_guard lock(this->mutex);
        this->tasks.push_back(std::move(cleanup));
    }

    bool runOne()
    {
        Task task;
        {
            std::lock_guard lock(this->mutex);
            if (this->tasks.empty())
            {
                return false;
            }
            task = std::move(this->tasks.front());
            this->tasks.erase(this->tasks.begin());
        }
        task();
        return true;
    }

    void runAll()
    {
        while (this->runOne())
        {
        }
    }

    bool owner = true;
    bool accept = true;
    std::mutex mutex;
    std::vector<Task> tasks;
};

struct PendingTransportState {
    std::atomic_bool active{true};
    std::atomic_int cancellations{0};
};

class PendingTransportHandle final : public TransportHandle
{
public:
    explicit PendingTransportHandle(
        std::shared_ptr<PendingTransportState> state)
        : state_(std::move(state))
    {
    }

    ~PendingTransportHandle() override
    {
        this->cancel();
    }

    void cancel() noexcept override
    {
        if (this->state_ &&
            this->state_->active.exchange(false, std::memory_order_acq_rel))
        {
            ++this->state_->cancellations;
        }
    }

    bool active() const noexcept override
    {
        return this->state_ &&
               this->state_->active.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<PendingTransportState> state_;
};

class PendingTransport final : public Transport
{
public:
    std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) override
    {
        this->requests.push_back(std::move(request));
        this->callbacks.push_back(std::move(callbacks));
        auto state = std::make_shared<PendingTransportState>();
        this->states.push_back(state);
        return std::make_unique<PendingTransportHandle>(std::move(state));
    }

    std::vector<TransportRequest> requests;
    std::vector<TransportCallbacks> callbacks;
    std::vector<std::shared_ptr<PendingTransportState>> states;

    void completeEmbedOffline(std::size_t index)
    {
        this->complete(index, ExpectedMediaType::Json,
                       QByteArrayLiteral("application/json"),
                       QByteArrayLiteral(R"({"vid":null,"unavailable":true})"));
    }

    void completeEmbedLive(std::size_t index)
    {
        this->complete(
            index, ExpectedMediaType::Json,
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral(
                R"({"vid":"12345","title":"Live video","channel_title":"Live channel"})"));
    }

    void completeChannelLive(std::size_t index)
    {
        this->complete(
            index, ExpectedMediaType::Html,
            QByteArrayLiteral("text/html; charset=utf-8"),
            QByteArrayLiteral(
                R"(<html><head><title>Live channel</title><script type="application/ld+json">{"embedUrl":"https://rumble.com/embed/vpickerlive"}</script></head><body><div class="videostream" duration="0"></div></body></html>)"));
    }

    void completeChannelOffline(std::size_t index)
    {
        this->complete(index, ExpectedMediaType::Html,
                       QByteArrayLiteral("text/html; charset=utf-8"),
                       QByteArrayLiteral(
                           "<html><head><title>Offline channel</title></head>"
                           "<body></body></html>"));
    }

    void completeEmoteCatalog(std::size_t index)
    {
        this->complete(index, ExpectedMediaType::Json,
                       QByteArrayLiteral("application/json"),
                       QByteArrayLiteral(R"({"data":{"items":[]}})"));
    }

private:
    void complete(std::size_t index, ExpectedMediaType expectedMediaType,
                  QByteArray contentType, QByteArray body)
    {
        ASSERT_LT(index, this->callbacks.size());
        ASSERT_LT(index, this->requests.size());
        ASSERT_EQ(this->requests[index].expectedMediaType, expectedMediaType);
        ASSERT_TRUE(this->states[index]->active.exchange(false));
        this->callbacks[index].onHead(ResponseHead{
            .status = 200,
            .headers =
                {
                    Header{QByteArrayLiteral("Content-Type"),
                           std::move(contentType)},
                },
        });
        this->callbacks[index].onBodyChunk(std::move(body));
        this->callbacks[index].onComplete();
    }
};

class PendingScheduledTask final : public ScheduledTask
{
public:
    void cancel() noexcept override
    {
        this->active_ = false;
    }

    bool active() const noexcept override
    {
        return this->active_;
    }

private:
    bool active_ = true;
};

class PendingScheduler final : public RumbleScheduler
{
public:
    std::int64_t nowMs() const noexcept override
    {
        return 0;
    }

    std::unique_ptr<ScheduledTask> scheduleAfter(std::int64_t delayMs,
                                                 Callback callback) override
    {
        if (this->throwOnSchedule)
        {
            throw std::runtime_error("scheduler unavailable");
        }
        if (!this->accept)
        {
            return {};
        }
        this->delays.push_back(delayMs);
        this->callbacks.push_back(std::move(callback));
        return std::make_unique<PendingScheduledTask>();
    }

    std::uint64_t randomBelow(std::uint64_t) override
    {
        return 0;
    }

    std::vector<Callback> callbacks;
    std::vector<std::int64_t> delays;
    bool accept = true;
    bool throwOnSchedule = false;
};

class DescriptorTwitchIrcServer final : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &name) override
    {
        return std::make_shared<Channel>(name, Channel::Type::Twitch);
    }

    ChannelPtr getChannelOrEmpty(const QString &name) override
    {
        return std::make_shared<Channel>(name, Channel::Type::Misc);
    }
};

class TestApplication final : public mock::BaseApplication
{
public:
    RumbleApplicationController *getRumble() override
    {
        return this->rumble;
    }

    RumbleApplicationController *rumble = nullptr;

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    HotkeyController hotkeys;
    DescriptorTwitchIrcServer twitch;
};

class DisplayChannel final : public Channel
{
public:
    DisplayChannel(QString name, QString display)
        : Channel(std::move(name), Type::Twitch)
        , display_(std::move(display))
    {
    }

    const QString &getDisplayName() const override
    {
        return this->display_;
    }

private:
    QString display_;
};

struct Fixture {
    Fixture()
        : api(transport,
              [this](std::function<void()> task) {
                  std::ignore = this->dispatcher->dispatch(std::move(task));
              })
        , controller(api, scheduler, dispatcher)
    {
        this->application.rumble = &this->controller;
    }

    TestApplication application;
    std::shared_ptr<QueueDispatcher> dispatcher =
        std::make_shared<QueueDispatcher>();
    PendingTransport transport;
    PendingScheduler scheduler;
    RumbleApi api;
    RumbleApplicationController controller;
};

QLineEdit *rumbleEditor(SelectChannelDialog &dialog)
{
    for (auto *editor : dialog.findChildren<QLineEdit *>())
    {
        if (editor->placeholderText().contains(QStringLiteral("rumble.com")))
        {
            return editor;
        }
    }
    return nullptr;
}

MicroNotebook *microNotebook(QWidget &root)
{
    for (auto *widget : root.findChildren<QWidget *>())
    {
        if (auto *notebook = dynamic_cast<MicroNotebook *>(widget))
        {
            return notebook;
        }
    }
    return nullptr;
}

void selectRumblePage(SelectChannelDialog &dialog, QLineEdit *editor)
{
    auto *notebook = microNotebook(dialog);
    ASSERT_NE(notebook, nullptr);
    ASSERT_NE(editor, nullptr);
    notebook->select(editor->parentWidget());
}

QPushButton *okButton(SelectChannelDialog &dialog)
{
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    return buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
}

QString allLabelText(const QWidget &widget)
{
    QString result;
    for (const auto *label : widget.findChildren<QLabel *>())
    {
        result += label->text();
        result += u'\n';
    }
    return result;
}

}  // namespace

TEST(RumbleApplicationLocator, CanonicalizesOnlyPublicPersistableLocators)
{
    auto channel = RumbleLayoutLocator::fromUserInput(QStringLiteral(
        "https://WWW.RUMBLE.COM/c/SomeChannel/live?token=drop#x"));
    ASSERT_TRUE(channel);
    EXPECT_EQ(channel->canonicalUrl(),
              QStringLiteral("https://rumble.com/c/somechannel"));

    auto embed = RumbleLayoutLocator::fromUserInput(
        QStringLiteral("https://rumble.com/embed/vabc123?secret=drop"));
    ASSERT_TRUE(embed);
    EXPECT_EQ(embed->canonicalUrl(),
              QStringLiteral("https://rumble.com/embed/vabc123"));

    auto page = RumbleLayoutLocator::fromUserInput(
        QStringLiteral("https://rumble.com/vabc123-title.html?secret=drop"));
    ASSERT_TRUE(page);
    EXPECT_EQ(page->canonicalUrl(),
              QStringLiteral("https://rumble.com/vabc123-title.html"));

    auto stream = RumbleLayoutLocator::fromUserInput(QStringLiteral("12345"));
    ASSERT_FALSE(stream);
    EXPECT_EQ(stream.error(),
              RumbleLayoutLocatorError::DirectStreamNotPersistable);
    EXPECT_FALSE(RumbleLayoutLocator::fromUserInput(
        QStringLiteral("https://example.com/c/not-rumble?secret=sentinel")));

    auto bare =
        RumbleLayoutLocator::fromUserInput(QStringLiteral("somechannel"));
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->canonicalUrl(),
              QStringLiteral("https://rumble.com/c/somechannel"));
    EXPECT_FALSE(
        RumbleLayoutLocator::fromPersisted(QStringLiteral("somechannel")));
    EXPECT_FALSE(RumbleLayoutLocator::fromPersisted(
        QStringLiteral("http://rumble.com/c/somechannel")));
    auto persisted = RumbleLayoutLocator::fromPersisted(QStringLiteral(
        "https://rumble.com/c/somechannel?secret=drop#fragment"));
    ASSERT_TRUE(persisted);
    EXPECT_EQ(persisted->canonicalUrl(),
              QStringLiteral("https://rumble.com/c/somechannel"));
}

TEST(RumbleApplicationLocator, PlaceholderFactoryRevalidatesItsBoundary)
{
    auto safe = makeRumbleLayoutPlaceholder(
        QStringLiteral("https://rumble.com/embed/vsafe?secret=drop"));
    ASSERT_TRUE(safe.layoutIdentity());
    EXPECT_EQ(safe.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/embed/vsafe"));

    auto unsafe = makeRumbleLayoutPlaceholder(
        QStringLiteral("https://example.com/c/no?secret=must-not-be-retained"));
    ASSERT_TRUE(unsafe.layoutIdentity());
    EXPECT_TRUE(unsafe.layoutIdentity()->locator.isEmpty());
    EXPECT_FALSE(unsafe.get()->getName().contains(
        QStringLiteral("must-not-be-retained")));

    auto runtimeLike =
        makeRumbleLayoutPlaceholder(QStringLiteral("rumble-secret-runtime-id"));
    ASSERT_TRUE(runtimeLike.layoutIdentity());
    EXPECT_TRUE(runtimeLike.layoutIdentity()->locator.isEmpty());
}

TEST(RumbleApplicationLifecycle,
     MissingControllerRoutesTopLevelAndNestedViewsToPickerRepair)
{
    TestApplication application;
    auto topLevel = makeRumbleLayoutPlaceholder(
        QStringLiteral("https://rumble.com/embed/vrepair"));
    EXPECT_TRUE(rumbleLayoutNeedsPickerRepair(topLevel, nullptr));

    const MultiChannel::Spec spec{
        .platform = MultiChannel::Platform::Rumble,
        .name = QStringLiteral("https://rumble.com/embed/vrepair"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator = QStringLiteral("https://rumble.com/embed/vrepair"),
            },
    };
    auto multi = std::make_shared<MultiChannel>(
        std::span<const MultiChannel::Spec>(&spec, 1),
        MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [](const MultiChannel::Spec &child) {
            return makeRumbleLayoutPlaceholder(child.name).get();
        });
    IndirectChannel nested(multi, Channel::Type::Multi);
    EXPECT_TRUE(rumbleLayoutNeedsPickerRepair(nested, nullptr));
}

TEST(RumbleApplicationIdentity, SharedCopiesResetButSeparateViewsStayDistinct)
{
    TestApplication application;
    auto firstChannel = std::make_shared<Channel>(QStringLiteral("first"),
                                                  Channel::Type::Rumble);
    auto replacement = std::make_shared<Channel>(QStringLiteral("replacement"),
                                                 Channel::Type::Rumble);
    const ChannelLayoutIdentity firstIdentity{
        .platform = QStringLiteral("rumble"),
        .locator = QStringLiteral("https://rumble.com/embed/vsame"),
    };
    IndirectChannel first(firstChannel, Channel::Type::Rumble, firstIdentity);
    auto sharedCopy = first;
    IndirectChannel separate(
        firstChannel, Channel::Type::Rumble,
        ChannelLayoutIdentity{
            .platform = QStringLiteral("rumble"),
            .locator = QStringLiteral("https://rumble.com/vsame-title.html"),
        });

    sharedCopy.reset(replacement);
    EXPECT_EQ(first.get(), replacement);
    EXPECT_EQ(separate.get(), firstChannel);
    ASSERT_TRUE(first.layoutIdentity());
    ASSERT_TRUE(separate.layoutIdentity());
    EXPECT_EQ(first.layoutIdentity()->locator, firstIdentity.locator);
    EXPECT_NE(first.layoutIdentity()->locator,
              separate.layoutIdentity()->locator);
}

TEST(RumbleApplicationLifecycle,
     KeyEquivalentConcurrentViewsShareOneConnectionButKeepLocators)
{
    Fixture fixture;
    IndirectChannel inconsistentMulti(
        std::make_shared<Channel>(QStringLiteral("not-a-multi-runtime"),
                                  Channel::Type::Multi),
        Channel::Type::Multi);
    EXPECT_TRUE(
        rumbleLayoutNeedsPickerRepair(inconsistentMulti, &fixture.controller));

    auto embed = fixture.controller.restore(
        QStringLiteral("https://rumble.com/embed/vsame?drop=1"));
    auto page = fixture.controller.restore(
        QStringLiteral("https://rumble.com/vsame-title.html?drop=2"));

    EXPECT_EQ(embed.get(), page.get());
    ASSERT_TRUE(embed.layoutIdentity());
    ASSERT_TRUE(page.layoutIdentity());
    EXPECT_EQ(embed.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/embed/vsame"));
    EXPECT_EQ(page.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/vsame-title.html"));
    EXPECT_NE(embed.layoutIdentity()->locator, page.layoutIdentity()->locator);
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
}

TEST(RumbleApplicationLifecycle,
     CancellingOneSharedLifecycleSubscriberDoesNotCancelTheOther)
{
    Fixture fixture;
    int cancelledCallbacks = 0;
    int retainedCallbacks = 0;
    auto cancelled = fixture.controller.resolve(
        QStringLiteral("https://rumble.com/embed/vsubscribers"), [&](auto) {
            ++cancelledCallbacks;
        });
    auto retained = fixture.controller.resolve(
        QStringLiteral("https://rumble.com/embed/vsubscribers"),
        [&](auto result) {
            EXPECT_TRUE(result);
            ++retainedCallbacks;
        });
    ASSERT_EQ(fixture.transport.requests.size(), 1U);

    cancelled.cancel();
    EXPECT_FALSE(cancelled.active());
    EXPECT_TRUE(retained.active());
    fixture.transport.completeEmbedOffline(0);
    fixture.dispatcher->runAll();

    EXPECT_EQ(cancelledCallbacks, 0);
    EXPECT_EQ(retainedCallbacks, 1);
    EXPECT_FALSE(retained.active());
}

TEST(RumbleApplicationLifecycle,
     CachedFailureStartsFreshZeroDelayGenerationBeforeCompleting)
{
    Fixture fixture;
    auto restored = fixture.controller.restore(
        QStringLiteral("https://rumble.com/embed/vretry"));
    auto runtime = std::dynamic_pointer_cast<RumbleChannel>(restored.get());
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
    ASSERT_TRUE(runtime->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure(RumbleFailureCategory::Resolution,
                      RumbleFailureCode::Unavailable,
                      RumbleOperatorText::ResolutionUnavailable)));

    int callbacks = 0;
    bool succeeded = false;
    auto request = fixture.controller.resolve(
        QStringLiteral("https://rumble.com/embed/vretry"), [&](auto result) {
            ++callbacks;
            succeeded = result.has_value();
        });
    EXPECT_TRUE(request.active());
    EXPECT_EQ(callbacks, 0);
    ASSERT_EQ(fixture.scheduler.callbacks.size(), 1U);
    ASSERT_EQ(fixture.scheduler.delays.size(), 1U);
    EXPECT_EQ(fixture.scheduler.delays.front(), 0);

    fixture.scheduler.callbacks.front()();
    ASSERT_EQ(fixture.transport.requests.size(), 2U);
    EXPECT_EQ(callbacks, 0);
    fixture.transport.completeEmbedOffline(1);
    fixture.dispatcher->runAll();
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(succeeded);
    EXPECT_FALSE(request.active());
}

TEST(RumbleApplicationLifecycle, PickerFailureIncludesSafeDiagnosticCode)
{
    Fixture fixture;
    const auto locator =
        QStringLiteral("https://rumble.com/embed/vdiagnostic");
    auto view = fixture.controller.restore(locator);
    auto runtime = std::dynamic_pointer_cast<RumbleChannel>(view.get());
    ASSERT_NE(runtime, nullptr);

    std::optional<RumbleLayoutResolveError> error;
    auto request = fixture.controller.resolve(locator, [&](auto result) {
        ASSERT_FALSE(result);
        error = result.error();
    });
    ASSERT_TRUE(runtime->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure(RumbleFailureCategory::Protocol,
                      RumbleFailureCode::MalformedResponse,
                      RumbleOperatorText::ResponseLimitExceeded,
                      QStringLiteral("sse_body_limit"))));
    fixture.dispatcher->runAll();

    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, RumbleLayoutResolveErrorCode::ResolutionFailed);
    EXPECT_EQ(error->userMessage,
              QStringLiteral(
                  "Rumble sent a response Chatterino could not process."));
    EXPECT_FALSE(request.active());
}

TEST(RumbleApplicationLifecycle,
     CachedFailureSchedulerFailureCompletesExactlyOnceWithTypedError)
{
    const auto exercise = [](bool throwOnSchedule) {
        Fixture fixture;
        auto restored = fixture.controller.restore(
            QStringLiteral("https://rumble.com/embed/vretryscheduler"));
        auto runtime = std::dynamic_pointer_cast<RumbleChannel>(restored.get());
        ASSERT_NE(runtime, nullptr);
        ASSERT_TRUE(runtime->transitionTo(
            RumbleChannelState::Failed,
            RumbleFailure(RumbleFailureCategory::Resolution,
                          RumbleFailureCode::Unavailable,
                          RumbleOperatorText::ResolutionUnavailable)));
        fixture.scheduler.accept = false;
        fixture.scheduler.throwOnSchedule = throwOnSchedule;

        int callbacks = 0;
        for (int attempt = 1; attempt <= 2; ++attempt)
        {
            std::optional<RumbleLayoutResolveError> error;
            auto request = fixture.controller.resolve(
                QStringLiteral("https://rumble.com/embed/vretryscheduler"),
                [&](auto result) {
                    ++callbacks;
                    ASSERT_FALSE(result);
                    error = result.error();
                });

            EXPECT_TRUE(request.active());
            EXPECT_EQ(callbacks, attempt - 1);
            fixture.dispatcher->runAll();
            EXPECT_EQ(callbacks, attempt);
            EXPECT_FALSE(request.active());
            ASSERT_TRUE(error);
            EXPECT_EQ(error->code,
                      RumbleLayoutResolveErrorCode::ResolutionFailed);
        }
        fixture.dispatcher->runAll();
        EXPECT_EQ(callbacks, 2);
    };

    exercise(false);
    exercise(true);
}

TEST(RumbleApplicationLifecycle,
     DeferredFailureCallbackCanResolveAgainWithoutReentrantHang)
{
    Fixture fixture;
    const auto locator =
        QStringLiteral("https://rumble.com/embed/vreentrantretry");
    auto view = fixture.controller.restore(locator);
    auto runtime = std::dynamic_pointer_cast<RumbleChannel>(view.get());
    ASSERT_NE(runtime, nullptr);

    int firstCallbacks = 0;
    int secondCallbacks = 0;
    RumbleApplicationController::Request second;
    auto first = fixture.controller.resolve(locator, [&](auto result) {
        ++firstCallbacks;
        EXPECT_FALSE(result);
        second = fixture.controller.resolve(locator, [&](auto retryResult) {
            ++secondCallbacks;
            EXPECT_FALSE(retryResult);
        });
    });
    ASSERT_TRUE(runtime->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure(RumbleFailureCategory::Resolution,
                      RumbleFailureCode::Unavailable,
                      RumbleOperatorText::ResolutionUnavailable)));
    fixture.scheduler.accept = false;

    EXPECT_TRUE(first.active());
    EXPECT_EQ(firstCallbacks, 0);
    EXPECT_EQ(secondCallbacks, 0);
    fixture.dispatcher->runAll();

    EXPECT_EQ(firstCallbacks, 1);
    EXPECT_EQ(secondCallbacks, 1);
    EXPECT_FALSE(first.active());
    EXPECT_FALSE(second.active());
    fixture.dispatcher->runAll();
    EXPECT_EQ(firstCallbacks, 1);
    EXPECT_EQ(secondCallbacks, 1);
}

TEST(RumbleProviderLifetime, RejectedTerminalDispatchCancelsWaiter)
{
    Fixture fixture;
    const auto locator =
        QStringLiteral("https://rumble.com/embed/vterminaldispatch");
    auto view = fixture.controller.restore(locator);
    auto runtime = std::dynamic_pointer_cast<RumbleChannel>(view.get());
    ASSERT_NE(runtime, nullptr);
    int callbacks = 0;
    auto request = fixture.controller.resolve(locator, [&](auto) {
        ++callbacks;
    });

    fixture.dispatcher->accept = false;
    ASSERT_TRUE(runtime->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure(RumbleFailureCategory::Resolution,
                      RumbleFailureCode::Unavailable,
                      RumbleOperatorText::ResolutionUnavailable)));

    EXPECT_FALSE(request.active());
    EXPECT_EQ(callbacks, 0);
}

TEST(RumbleChannelPicker,
     NewAndEditFlowsKeepCanonicalLocatorDespitePrecheckedTwitch)
{
    Fixture fixture;
    SelectChannelDialog first;
    auto *firstEditor = rumbleEditor(first);
    selectRumblePage(first, firstEditor);
    firstEditor->setText(QStringLiteral("pickslug"));
    auto *firstOk = okButton(first);
    ASSERT_NE(firstOk, nullptr);
    firstOk->click();
    EXPECT_FALSE(firstEditor->isEnabled());
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
    fixture.transport.completeChannelOffline(0);
    fixture.dispatcher->runAll();

    ASSERT_TRUE(first.hasSeletedChannel());
    auto selected = first.getSelectedChannel();
    EXPECT_EQ(selected.getType(), Channel::Type::Rumble);
    ASSERT_TRUE(selected.layoutIdentity());
    EXPECT_EQ(selected.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/c/pickslug"));

    SelectChannelDialog edit;
    edit.setSelectedChannel(selected);
    auto *editEditor = rumbleEditor(edit);
    ASSERT_NE(editEditor, nullptr);
    EXPECT_EQ(editEditor->text(), selected.layoutIdentity()->locator);
    editEditor->setText(
        QStringLiteral("https://rumble.com/embed/vreplacement?drop=1"));
    auto *editOk = okButton(edit);
    ASSERT_NE(editOk, nullptr);
    editOk->click();
    ASSERT_EQ(fixture.transport.requests.size(), 2U);
    fixture.transport.completeEmbedOffline(1);
    fixture.dispatcher->runAll();
    ASSERT_TRUE(edit.hasSeletedChannel());
    auto replacement = edit.getSelectedChannel();
    ASSERT_TRUE(replacement.layoutIdentity());
    EXPECT_EQ(replacement.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/embed/vreplacement"));
}

TEST(RumbleChannelPicker, LiveResolutionAcceptsBeforeAnonymousStreamInit)
{
    Fixture fixture;
    SelectChannelDialog dialog;
    auto *editor = rumbleEditor(dialog);
    selectRumblePage(dialog, editor);
    editor->setText(QStringLiteral("pickerlive"));
    auto *ok = okButton(dialog);
    ASSERT_NE(ok, nullptr);

    ok->click();
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
    fixture.transport.completeChannelLive(0);
    fixture.dispatcher->runAll();
    ASSERT_EQ(fixture.transport.requests.size(), 2U);
    EXPECT_FALSE(dialog.hasSeletedChannel());

    fixture.transport.completeEmbedLive(1);
    fixture.dispatcher->runAll();

    ASSERT_EQ(fixture.transport.requests.size(), 3U);
    ASSERT_TRUE(dialog.hasSeletedChannel());
    const auto selected = dialog.getSelectedChannel();
    ASSERT_TRUE(selected.layoutIdentity());
    EXPECT_EQ(selected.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/c/pickerlive"));
    const auto runtime =
        std::dynamic_pointer_cast<RumbleChannel>(selected.get());
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->state(), RumbleChannelState::Connecting);
    ASSERT_EQ(fixture.transport.states.size(), 3U);
    EXPECT_TRUE(fixture.transport.states[2]->active.load());

    fixture.transport.completeEmoteCatalog(2);
    fixture.dispatcher->runAll();
    ASSERT_EQ(fixture.transport.requests.size(), 4U);
    EXPECT_TRUE(fixture.transport.states[3]->active.load());
}

TEST(RumbleChannelPicker, ErrorReenablesAcceptanceAndDestructionClosesGate)
{
    Fixture fixture;
    SelectChannelDialog invalid;
    auto *editor = rumbleEditor(invalid);
    selectRumblePage(invalid, editor);
    editor->setText(QStringLiteral("12345"));
    auto *ok = okButton(invalid);
    ASSERT_NE(ok, nullptr);
    ok->click();
    fixture.dispatcher->runAll();
    EXPECT_TRUE(editor->isEnabled());
    EXPECT_TRUE(microNotebook(invalid)->isEnabled());
    EXPECT_TRUE(ok->isEnabled());
    EXPECT_FALSE(invalid.hasSeletedChannel());

    auto *pending = new SelectChannelDialog;
    QPointer<SelectChannelDialog> guarded(pending);
    auto *pendingEditor = rumbleEditor(*pending);
    selectRumblePage(*pending, pendingEditor);
    pendingEditor->setText(
        QStringLiteral("https://rumble.com/embed/vdestroyed"));
    okButton(*pending)->click();
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
    delete pending;
    EXPECT_TRUE(guarded.isNull());
    fixture.transport.completeEmbedOffline(0);
    fixture.dispatcher->runAll();
}

TEST(RumbleChannelPicker, RejectedDispatchRestoresControlsWithSafeError)
{
    Fixture fixture;
    fixture.dispatcher->accept = false;
    SelectChannelDialog dialog;
    auto *editor = rumbleEditor(dialog);
    selectRumblePage(dialog, editor);
    editor->setText(QStringLiteral(
        "https://example.com/c/no?secret=must-not-be-displayed"));
    auto *ok = okButton(dialog);
    ASSERT_NE(ok, nullptr);

    ok->click();

    EXPECT_TRUE(editor->isEnabled());
    EXPECT_TRUE(microNotebook(dialog)->isEnabled());
    EXPECT_TRUE(ok->isEnabled());
    EXPECT_FALSE(dialog.hasSeletedChannel());
    const auto labels = allLabelText(dialog);
    EXPECT_TRUE(labels.contains(QStringLiteral("couldn't connect"),
                                Qt::CaseInsensitive));
    EXPECT_FALSE(labels.contains(QStringLiteral("must-not-be-displayed")));
}

TEST(RumbleChannelPicker,
     PendingResolveDisablesNavigationAndSuppressesChangedContext)
{
    Fixture fixture;
    SelectChannelDialog dialog;
    auto *editor = rumbleEditor(dialog);
    selectRumblePage(dialog, editor);
    auto *notebook = microNotebook(dialog);
    ASSERT_NE(notebook, nullptr);
    QWidget *twitchPage = nullptr;
    for (auto *radio : dialog.findChildren<QRadioButton *>())
    {
        if (radio->text() == QStringLiteral("Channel"))
        {
            twitchPage = radio->parentWidget();
            break;
        }
    }
    ASSERT_NE(twitchPage, nullptr);
    editor->setText(QStringLiteral("https://rumble.com/embed/vcontext"));
    okButton(dialog)->click();
    EXPECT_FALSE(notebook->isEnabled());
    ASSERT_EQ(fixture.transport.requests.size(), 1U);

    // Disabled navigation prevents this through the UI. Force the context
    // change programmatically to verify the completion gate as well.
    notebook->select(twitchPage);
    fixture.transport.completeEmbedOffline(0);
    fixture.dispatcher->runAll();

    EXPECT_TRUE(notebook->isEnabled());
    EXPECT_TRUE(editor->isEnabled());
    EXPECT_FALSE(dialog.hasSeletedChannel());
}

TEST(RumbleChannelPicker, NestedMultiPickerResolvesAndCancelsIndependently)
{
    Fixture fixture;
    SelectChannelDialog parent;
    auto *multiView = parent.findChild<QListWidget *>();
    ASSERT_NE(multiView, nullptr);
    auto *notebook = microNotebook(parent);
    ASSERT_NE(notebook, nullptr);
    notebook->select(multiView->parentWidget());

    QPushButton *add = nullptr;
    for (auto *button : parent.findChildren<QPushButton *>())
    {
        if (button->text() == QStringLiteral("Add"))
        {
            add = button;
            break;
        }
    }
    ASSERT_NE(add, nullptr);
    add->click();

    QWidget *nested = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
    {
        if (widget->windowTitle() == QStringLiteral("Add Channel"))
        {
            nested = widget;
            break;
        }
    }
    ASSERT_NE(nested, nullptr);
    auto *platform = nested->findChild<QComboBox *>();
    auto *name = nested->findChild<QLineEdit *>();
    auto *buttons = nested->findChild<QDialogButtonBox *>();
    ASSERT_NE(platform, nullptr);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(buttons, nullptr);
    platform->setCurrentIndex(platform->findText(QStringLiteral("Rumble")));
    name->setText(QStringLiteral("https://rumble.com/embed/vnested"));
    buttons->button(QDialogButtonBox::Ok)->click();
    ASSERT_EQ(fixture.transport.requests.size(), 1U);
    fixture.transport.completeEmbedOffline(0);
    fixture.dispatcher->runAll();
    ASSERT_EQ(multiView->count(), 1);
    auto data = multiView->item(0)->data(Qt::UserRole);
    auto *spec = get_if<MultiChannel::Spec>(&data);
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(spec->layoutIdentity);
    EXPECT_EQ(spec->layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vnested"));

    add->click();
    nested = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
    {
        if (widget->windowTitle() == QStringLiteral("Add Channel") &&
            widget->isVisible())
        {
            nested = widget;
            break;
        }
    }
    ASSERT_NE(nested, nullptr);
    platform = nested->findChild<QComboBox *>();
    name = nested->findChild<QLineEdit *>();
    buttons = nested->findChild<QDialogButtonBox *>();
    platform->setCurrentIndex(platform->findText(QStringLiteral("Rumble")));
    name->setText(QStringLiteral("https://rumble.com/embed/vcancelled"));
    buttons->button(QDialogButtonBox::Ok)->click();
    ASSERT_EQ(fixture.transport.requests.size(), 2U);
    nested->close();
    fixture.transport.completeEmbedOffline(1);
    fixture.dispatcher->runAll();
    EXPECT_EQ(multiView->count(), 1);
}

TEST(RumbleApplicationLifecycle, ShutdownCancelsPendingResolutionAndSubscriber)
{
    Fixture fixture;
    auto view = fixture.controller.restore(
        QStringLiteral("https://rumble.com/embed/vpending"));
    auto runtime = std::dynamic_pointer_cast<RumbleChannel>(view.get());
    ASSERT_NE(runtime, nullptr);
    int callbacks = 0;
    auto request = fixture.controller.resolve(
        QStringLiteral("https://rumble.com/embed/vpending"), [&](auto) {
            ++callbacks;
        });
    ASSERT_TRUE(request.active());
    ASSERT_EQ(fixture.transport.states.size(), 1U);

    fixture.controller.beginShutdown();
    fixture.controller.beginShutdown();
    EXPECT_FALSE(request.active());
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(fixture.transport.states.front()->cancellations.load(), 1);
    EXPECT_NE(runtime->state(), RumbleChannelState::Closed);
    ASSERT_TRUE(view.layoutIdentity());
    QJsonObject encoded;
    WindowManager::encodeChannel(view, encoded);
    EXPECT_EQ(encoded.value(QStringLiteral("locator")).toString(),
              QStringLiteral("https://rumble.com/embed/vpending"));

    fixture.controller.shutdown();
    fixture.controller.shutdown();
    EXPECT_EQ(runtime->state(), RumbleChannelState::Closed);
    fixture.dispatcher->runAll();
    EXPECT_EQ(callbacks, 0);
}

TEST(RumbleApplicationLifecycle,
     ValidLocatorSurvivesUnavailableProviderAsTypedPlaceholder)
{
    TestApplication application;
    auto dispatcher = std::make_shared<QueueDispatcher>();
    PendingTransport transport;
    PendingScheduler scheduler;
    RumbleApi api(transport, [dispatcher](auto task) {
        std::ignore = dispatcher->dispatch(std::move(task));
    });
    RumbleApplicationController controller(
        api, scheduler, dispatcher, [](const RumbleLayoutLocator &) {
            return std::shared_ptr<RumbleChannel>{};
        });
    application.rumble = &controller;

    auto view = controller.restore(
        QStringLiteral("https://rumble.com/embed/vsafe?drop=1"));
    EXPECT_EQ(view.getType(), Channel::Type::Rumble);
    ASSERT_TRUE(view.layoutIdentity());
    EXPECT_EQ(view.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/embed/vsafe"));
    EXPECT_TRUE(view.get()->canReconnect());
}

TEST(RumbleProviderLifetime, RejectedDispatchRetiresInvalidRequest)
{
    Fixture fixture;
    fixture.dispatcher->accept = false;
    int callbacks = 0;
    auto request =
        fixture.controller.resolve(QStringLiteral("12345"), [&](auto) {
            ++callbacks;
        });
    EXPECT_FALSE(request.active());
    EXPECT_EQ(callbacks, 0);
}

TEST(RumbleProviderLifetime, ShutdownImmediatelyRetiresQueuedFailure)
{
    Fixture fixture;
    int callbacks = 0;
    auto request = fixture.controller.resolve(
        QStringLiteral("not a valid locator"), [&](auto) {
            ++callbacks;
        });
    ASSERT_TRUE(request.active());
    fixture.controller.beginShutdown();
    EXPECT_FALSE(request.active());
    fixture.dispatcher->runAll();
    EXPECT_EQ(callbacks, 0);
}

TEST(RumbleProviderLifetime, CancelAfterCallbackClaimWaitsForTheRunningCallback)
{
    Fixture fixture;
    std::mutex mutex;
    std::condition_variable changed;
    bool callbackEntered = false;
    bool releaseCallback = false;
    bool cancelStarted = false;
    std::uint64_t sequence = 0;
    std::uint64_t callbackFinishedAt = 0;
    std::uint64_t cancelFinishedAt = 0;

    auto request = fixture.controller.resolve(
        QStringLiteral("not a valid locator"), [&](auto) {
            std::unique_lock lock(mutex);
            callbackEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] {
                return releaseCallback;
            });
            callbackFinishedAt = ++sequence;
        });

    std::thread delivery([&] {
        fixture.dispatcher->runAll();
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return callbackEntered;
        });
    }

    std::thread cancellation([&] {
        {
            std::lock_guard lock(mutex);
            cancelStarted = true;
        }
        changed.notify_all();
        request.cancel();
        {
            std::lock_guard lock(mutex);
            cancelFinishedAt = ++sequence;
        }
        changed.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return cancelStarted;
        });
        releaseCallback = true;
    }
    changed.notify_all();

    delivery.join();
    cancellation.join();
    EXPECT_EQ(callbackFinishedAt, 1U);
    EXPECT_EQ(cancelFinishedAt, 2U);
}

TEST(RumbleProviderLifetime, StartFailureCompletesWithTypedError)
{
    TestApplication application;
    auto channelDispatcher = std::make_shared<QueueDispatcher>();
    RumbleChannelProvider externalProvider(channelDispatcher);
    auto channel = externalProvider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                                QStringLiteral("vstart"));
    ASSERT_TRUE(channel);

    auto controllerDispatcher = std::make_shared<QueueDispatcher>();
    controllerDispatcher->owner = false;
    PendingTransport transport;
    PendingScheduler scheduler;
    RumbleApi api(transport, [controllerDispatcher](auto task) {
        std::ignore = controllerDispatcher->dispatch(std::move(task));
    });
    RumbleApplicationController controller(
        api, scheduler, controllerDispatcher,
        [channel = *channel](const RumbleLayoutLocator &) {
            return channel;
        });
    application.rumble = &controller;

    std::optional<RumbleLayoutResolveError> error;
    auto request = controller.resolve(
        QStringLiteral("https://rumble.com/embed/vstart"), [&](auto result) {
            ASSERT_FALSE(result);
            error = result.error();
        });
    controllerDispatcher->runAll();
    EXPECT_FALSE(request.active());
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, RumbleLayoutResolveErrorCode::ResolutionFailed);
}

TEST(RumbleWindowDescriptor, ValidatesRumbleAtFinalSerializationBoundary)
{
    TestApplication application;
    ChildChannelDescriptor unsafe{
        .platform = QStringLiteral("Rumble"),
        .channelName = QStringLiteral("rumble-secret-runtime-id"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator = QStringLiteral(
                    "https://example.com/c/no?secret=secret-sentinel"),
            },
    };
    const auto unsafeJson = unsafe.toJson();
    const auto unsafeBytes = QJsonDocument(unsafeJson).toJson();
    EXPECT_FALSE(unsafeJson.contains(QStringLiteral("channel")));
    EXPECT_FALSE(unsafeJson.contains(QStringLiteral("locator")));
    EXPECT_FALSE(unsafeBytes.contains("secret-sentinel"));
    EXPECT_FALSE(unsafeBytes.contains("rumble-secret-runtime-id"));

    ChildChannelDescriptor twitch{
        .platform = QStringLiteral("Twitch"),
        .channelName = QStringLiteral("stable-login"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("unknown"),
                .locator = QStringLiteral("secret-sentinel"),
            },
    };
    const auto twitchJson = twitch.toJson();
    EXPECT_EQ(twitchJson.value(QStringLiteral("channel")).toString(),
              QStringLiteral("stable-login"));
    EXPECT_FALSE(twitchJson.contains(QStringLiteral("layoutIdentity")));
}

TEST(RumbleWindowDescriptor, RuntimeLikeBareValuesNeverBecomePersistedLocators)
{
    TestApplication application;
    const auto runtimeLike = QStringLiteral("rumble-secret-runtime-id");
    const QJsonObject topData{
        {QStringLiteral("type"), QStringLiteral("rumble")},
        {QStringLiteral("name"), runtimeLike},
        {QStringLiteral("layoutIdentity"),
         QJsonObject{
             {QStringLiteral("platform"), QStringLiteral("rumble")},
             {QStringLiteral("locator"), runtimeLike},
         }},
    };
    SplitDescriptor topDescriptor;
    SplitDescriptor::loadFromJSON(topDescriptor, QJsonObject{}, topData);
    ASSERT_TRUE(topDescriptor.layoutIdentity);
    EXPECT_TRUE(topDescriptor.channelName_.isEmpty());
    EXPECT_TRUE(topDescriptor.layoutIdentity->locator.isEmpty());
    auto topView = topDescriptor.decodeChannel();
    QJsonObject topEncoded;
    WindowManager::encodeChannel(topView, topEncoded);
    EXPECT_FALSE(topEncoded.contains(QStringLiteral("locator")));
    EXPECT_FALSE(
        QJsonDocument(topEncoded).toJson().contains(runtimeLike.toUtf8()));

    const auto child = ChildChannelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("platform"), QStringLiteral("Rumble")},
        {QStringLiteral("channel"), runtimeLike},
        {QStringLiteral("layoutIdentity"),
         QJsonObject{
             {QStringLiteral("platform"), QStringLiteral("rumble")},
             {QStringLiteral("locator"), runtimeLike},
         }},
    });
    auto childSpec = MultiChannel::Spec::fromDescriptor(child);
    EXPECT_TRUE(child.channelName.isEmpty());
    ASSERT_TRUE(childSpec);
    ASSERT_TRUE(childSpec->layoutIdentity);
    EXPECT_TRUE(childSpec->name.isEmpty());
    EXPECT_TRUE(childSpec->layoutIdentity->locator.isEmpty());
    const auto childJson = childSpec->descriptor().toJson();
    EXPECT_FALSE(childJson.contains(QStringLiteral("channel")));
    EXPECT_FALSE(childJson.contains(QStringLiteral("locator")));
    EXPECT_FALSE(
        QJsonDocument(childJson).toJson().contains(runtimeLike.toUtf8()));

    const auto genericUrl = QStringLiteral("https://rumble.com/embed/vruntime");
    const auto genericOnlyChild = ChildChannelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("platform"), QStringLiteral("Rumble")},
        {QStringLiteral("channel"), genericUrl},
    });
    EXPECT_TRUE(genericOnlyChild.channelName.isEmpty());
    auto genericOnlySpec = MultiChannel::Spec::fromDescriptor(genericOnlyChild);
    ASSERT_TRUE(genericOnlySpec);
    ASSERT_TRUE(genericOnlySpec->layoutIdentity);
    EXPECT_TRUE(genericOnlySpec->layoutIdentity->locator.isEmpty());
    EXPECT_FALSE(QJsonDocument(genericOnlySpec->descriptor().toJson())
                     .toJson()
                     .contains(genericUrl.toUtf8()));
}

TEST(RumbleWindowDescriptor,
     FullWindowTabAndNestedContainerRestoreUsesDedicatedLocators)
{
    TestApplication application;
    const QJsonObject topSplit{
        {QStringLiteral("type"), QStringLiteral("split")},
        {QStringLiteral("futureSplit"), QJsonArray{1, 2, 3}},
        {QStringLiteral("data"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("rumble")},
             {QStringLiteral("name"),
              QStringLiteral("https://rumble.com/embed/vruntime")},
             {QStringLiteral("locator"),
              QStringLiteral("https://rumble.com/c/top?drop=1")},
             {QStringLiteral("futureData"),
              QJsonObject{{QStringLiteral("ignored"), true}}},
         }},
    };
    const QJsonObject multiSplit{
        {QStringLiteral("type"), QStringLiteral("split")},
        {QStringLiteral("data"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("multi")},
             {QStringLiteral("activeIndex"), 1},
             {QStringLiteral("children"),
              QJsonArray{
                  QJsonObject{
                      {QStringLiteral("platform"), QStringLiteral("Rumble")},
                      {QStringLiteral("channel"),
                       QStringLiteral(
                           "https://rumble.com/embed/vruntime-child")},
                      {QStringLiteral("locator"),
                       QStringLiteral(
                           "https://rumble.com/embed/vchild?drop=1")},
                      {QStringLiteral("futureChild"),
                       QJsonObject{{QStringLiteral("ignored"), true}}},
                  },
                  QJsonObject{
                      {QStringLiteral("platform"), QStringLiteral("Twitch")},
                      {QStringLiteral("channel"),
                       QStringLiteral("stable-twitch-login")},
                  },
              }},
         }},
    };
    const QJsonObject nestedContainer{
        {QStringLiteral("type"), QStringLiteral("horizontal")},
        {QStringLiteral("futureContainer"),
         QJsonObject{{QStringLiteral("ignored"), true}}},
        {QStringLiteral("items"), QJsonArray{multiSplit}},
    };
    const QJsonObject rootContainer{
        {QStringLiteral("type"), QStringLiteral("vertical")},
        {QStringLiteral("items"), QJsonArray{topSplit, nestedContainer}},
    };
    const QJsonObject document{
        {QStringLiteral("windows"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("main")},
                 {QStringLiteral("futureWindow"),
                  QJsonObject{{QStringLiteral("ignored"), true}}},
                 {QStringLiteral("tabs"),
                  QJsonArray{
                      QJsonObject{
                          {QStringLiteral("title"),
                           QStringLiteral("Rumble restore")},
                          {QStringLiteral("selected"), true},
                          {QStringLiteral("futureTab"),
                           QJsonArray{QStringLiteral("ignored")}},
                          {QStringLiteral("splits2"), rootContainer},
                      },
                  }},
             },
         }},
    };

    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    const auto bytes = QJsonDocument(document).toJson();
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();

    const auto layout = WindowLayout::loadFromFile(file.fileName());
    ASSERT_EQ(layout.windows_.size(), 1U);
    ASSERT_EQ(layout.windows_.front().tabs_.size(), 1U);
    const auto &tab = layout.windows_.front().tabs_.front();
    EXPECT_EQ(tab.customTitle_, QStringLiteral("Rumble restore"));
    EXPECT_TRUE(tab.selected_);
    ASSERT_TRUE(tab.rootNode_);
    const auto *root = std::get_if<ContainerNodeDescriptor>(&*tab.rootNode_);
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->vertical_);
    ASSERT_EQ(root->items_.size(), 2U);

    const auto *top = std::get_if<SplitNodeDescriptor>(&root->items_[0]);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->type_, QStringLiteral("rumble"));
    EXPECT_TRUE(top->channelName_.isEmpty());
    ASSERT_TRUE(top->layoutIdentity);
    EXPECT_EQ(top->layoutIdentity->locator,
              QStringLiteral("https://rumble.com/c/top"));
    const auto topView = top->decodeChannel();
    EXPECT_EQ(topView.getType(), Channel::Type::Rumble);
    ASSERT_TRUE(topView.layoutIdentity());
    EXPECT_EQ(topView.layoutIdentity()->locator, top->layoutIdentity->locator);

    const auto *nested = std::get_if<ContainerNodeDescriptor>(&root->items_[1]);
    ASSERT_NE(nested, nullptr);
    EXPECT_FALSE(nested->vertical_);
    ASSERT_EQ(nested->items_.size(), 1U);
    const auto *multiDescriptor =
        std::get_if<SplitNodeDescriptor>(&nested->items_.front());
    ASSERT_NE(multiDescriptor, nullptr);
    EXPECT_EQ(multiDescriptor->type_, QStringLiteral("multi"));
    ASSERT_EQ(multiDescriptor->children.size(), 2U);
    EXPECT_TRUE(multiDescriptor->children[0].channelName.isEmpty());
    ASSERT_TRUE(multiDescriptor->children[0].layoutIdentity);
    EXPECT_EQ(multiDescriptor->children[0].layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vchild"));
    EXPECT_EQ(multiDescriptor->children[1].channelName,
              QStringLiteral("stable-twitch-login"));

    const auto multiView = multiDescriptor->decodeChannel();
    EXPECT_EQ(multiView.getType(), Channel::Type::Multi);
    const auto multi = std::dynamic_pointer_cast<MultiChannel>(multiView.get());
    ASSERT_NE(multi, nullptr);
    ASSERT_EQ(multi->channels().size(), 2U);
    EXPECT_EQ(multi->activeChannelIndex(), 1U);
    ASSERT_TRUE(multi->channels()[0].spec().layoutIdentity);
    EXPECT_EQ(multi->channels()[0].spec().layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vchild"));
    EXPECT_EQ(multi->channels()[1].spec().name,
              QStringLiteral("stable-twitch-login"));
}

TEST(RumbleWindowDescriptor,
     EveryPreExistingStableDescriptorTypeRetainsItsSemantics)
{
    TestApplication application;
    struct DescriptorCase {
        QStringView encoded;
        Channel::Type type;
    };
    const std::array cases{
        DescriptorCase{u"twitch", Channel::Type::Twitch},
        DescriptorCase{u"whispers", Channel::Type::TwitchWhispers},
        DescriptorCase{u"watching", Channel::Type::TwitchWatching},
        DescriptorCase{u"mentions", Channel::Type::TwitchMentions},
        DescriptorCase{u"live", Channel::Type::TwitchLive},
        DescriptorCase{u"automod", Channel::Type::TwitchAutomod},
        DescriptorCase{u"misc", Channel::Type::Misc},
        DescriptorCase{u"kick", Channel::Type::Kick},
        DescriptorCase{u"multi", Channel::Type::Multi},
    };

    for (const auto &entry : cases)
    {
        const auto encoded = entry.encoded.toString();
        SplitDescriptor descriptor;
        SplitDescriptor::loadFromJSON(
            descriptor,
            QJsonObject{
                {QStringLiteral("moderationMode"), true},
                {QStringLiteral("checkSpelling"), false},
            },
            QJsonObject{
                {QStringLiteral("type"), encoded},
                {QStringLiteral("name"), QStringLiteral("stable-name")},
                {QStringLiteral("channelID"), 101},
                {QStringLiteral("userID"), 202},
                {QStringLiteral("roomID"), 303},
                {QStringLiteral("children"), QJsonArray{}},
            });

        EXPECT_EQ(descriptor.type_, encoded);
        EXPECT_EQ(qmagicenum::enumCast<Channel::Type>(descriptor.type_),
                  entry.type);
        EXPECT_EQ(descriptor.channelName_, QStringLiteral("stable-name"));
        EXPECT_FALSE(descriptor.layoutIdentity);
        EXPECT_TRUE(descriptor.moderationMode_);
        ASSERT_TRUE(descriptor.spellCheckOverride);
        EXPECT_FALSE(*descriptor.spellCheckOverride);

        if (entry.type == Channel::Type::Kick)
        {
            EXPECT_EQ(descriptor.kickChannelID, 101U);
            EXPECT_EQ(descriptor.kickUserID, 202U);
            EXPECT_EQ(descriptor.kickRoomID, 303U);
            continue;
        }

        const auto decoded = descriptor.decodeChannel();
        switch (entry.type)
        {
            case Channel::Type::Twitch:
            case Channel::Type::Misc:
                EXPECT_EQ(decoded.getType(), entry.type);
                EXPECT_EQ(decoded.get()->getName(),
                          QStringLiteral("stable-name"));
                break;
            case Channel::Type::TwitchWhispers:
                EXPECT_EQ(decoded.get(),
                          application.twitch.getWhispersChannel());
                break;
            case Channel::Type::TwitchWatching:
                EXPECT_EQ(decoded.get(),
                          application.twitch.getWatchingChannel().get());
                break;
            case Channel::Type::TwitchMentions:
                EXPECT_EQ(decoded.get(),
                          application.twitch.getMentionsChannel());
                break;
            case Channel::Type::TwitchLive:
                EXPECT_EQ(decoded.get(), application.twitch.getLiveChannel());
                break;
            case Channel::Type::TwitchAutomod:
                EXPECT_EQ(decoded.get(),
                          application.twitch.getAutomodChannel());
                break;
            case Channel::Type::Multi:
                EXPECT_EQ(decoded.getType(), Channel::Type::Multi);
                break;
            case Channel::Type::Kick:
            case Channel::Type::Rumble:
            case Channel::Type::None:
            case Channel::Type::Direct:
            case Channel::Type::TwitchEnd:
                FAIL() << "unexpected descriptor case";
                break;
        }
    }
}

TEST(RumbleWindowDescriptor,
     MalformedNewIdentityFallsBackToValidatedLegacyLocatorOnly)
{
    const QJsonObject data{
        {QStringLiteral("type"), QStringLiteral("rumble")},
        {QStringLiteral("name"), QStringLiteral("rumble-secret-runtime-id")},
        {QStringLiteral("locator"),
         QStringLiteral("https://rumble.com/embed/vsafe?drop=1")},
        {QStringLiteral("layoutIdentity"),
         QJsonObject{
             {QStringLiteral("platform"), QStringLiteral("wrong")},
             {QStringLiteral("locator"), QStringLiteral("secret-sentinel")},
         }},
    };
    SplitDescriptor descriptor;
    SplitDescriptor::loadFromJSON(descriptor, QJsonObject{}, data);
    ASSERT_TRUE(descriptor.layoutIdentity);
    EXPECT_EQ(descriptor.layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vsafe"));

    auto missing = data;
    missing.remove(QStringLiteral("locator"));
    SplitDescriptor missingDescriptor;
    SplitDescriptor::loadFromJSON(missingDescriptor, QJsonObject{}, missing);
    ASSERT_TRUE(missingDescriptor.layoutIdentity);
    EXPECT_TRUE(missingDescriptor.layoutIdentity->locator.isEmpty());
}

TEST(RumbleWindowDescriptor,
     NoControllerRestorePreservesSafeLocatorAndRoundTrips)
{
    TestApplication application;
    const QJsonObject data{
        {QStringLiteral("type"), QStringLiteral("rumble")},
        {QStringLiteral("locator"),
         QStringLiteral("https://rumble.com/c/safe?drop=1")},
    };
    SplitDescriptor descriptor;
    SplitDescriptor::loadFromJSON(descriptor, QJsonObject{}, data);
    auto view = descriptor.decodeChannel();
    EXPECT_EQ(view.getType(), Channel::Type::Rumble);
    ASSERT_TRUE(view.layoutIdentity());
    EXPECT_EQ(view.layoutIdentity()->locator,
              QStringLiteral("https://rumble.com/c/safe"));
    EXPECT_TRUE(view.get()->canReconnect());

    QJsonObject encoded;
    WindowManager::encodeChannel(view, encoded);
    EXPECT_EQ(encoded.value(QStringLiteral("locator")).toString(),
              QStringLiteral("https://rumble.com/c/safe"));
    EXPECT_FALSE(
        QJsonDocument(encoded).toJson().contains("rumble-unavailable"));
}

TEST(RumbleWindowDescriptor, TopLevelEncodingNeverUsesRuntimeChannelName)
{
    TestApplication application;
    auto runtime = std::make_shared<Channel>(
        QStringLiteral("rumble-secret-runtime-id"), Channel::Type::Rumble);
    IndirectChannel view(
        runtime, Channel::Type::Rumble,
        ChannelLayoutIdentity{
            .platform = QStringLiteral("rumble"),
            .locator = QStringLiteral("https://rumble.com/c/safe?secret=drop"),
        });
    QJsonObject encoded;
    WindowManager::encodeChannel(view, encoded);
    const auto bytes = QJsonDocument(encoded).toJson();
    EXPECT_EQ(encoded.value(QStringLiteral("type")).toString(),
              QStringLiteral("rumble"));
    EXPECT_EQ(encoded.value(QStringLiteral("locator")).toString(),
              QStringLiteral("https://rumble.com/c/safe"));
    EXPECT_FALSE(bytes.contains("rumble-secret-runtime-id"));
    EXPECT_FALSE(bytes.contains("secret=drop"));
}

TEST(RumbleWindowDescriptor,
     MultiRetainsDistinctLocatorsAndBindsSharedRuntimeOnce)
{
    TestApplication application;
    auto dispatcher = std::make_shared<QueueDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto result = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                       QStringLiteral("vsame"));
    ASSERT_TRUE(result);
    auto runtime = *result;
    const std::array specs{
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/embed/vsame"),
            .layoutIdentity =
                ChannelLayoutIdentity{
                    .platform = QStringLiteral("rumble"),
                    .locator = QStringLiteral("https://rumble.com/embed/vsame"),
                },
        },
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/vsame-title.html"),
            .layoutIdentity =
                ChannelLayoutIdentity{
                    .platform = QStringLiteral("rumble"),
                    .locator =
                        QStringLiteral("https://rumble.com/vsame-title.html"),
                },
        },
    };
    MultiChannel multi(specs,
                       MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
                       [runtime](const auto &) {
                           return runtime;
                       });

    ASSERT_EQ(multi.channels().size(), 2U);
    EXPECT_EQ(multi.channels()[0].channel, multi.channels()[1].channel);
    EXPECT_TRUE(multi.channels()[0].primaryRuntimeChannel);
    EXPECT_FALSE(multi.channels()[1].primaryRuntimeChannel);
    EXPECT_NE(multi.channels()[0].spec().layoutIdentity->locator,
              multi.channels()[1].spec().layoutIdentity->locator);
    const auto firstJson = multi.channels()[0].descriptor().toJson();
    const auto secondJson = multi.channels()[1].descriptor().toJson();
    EXPECT_EQ(firstJson.value(QStringLiteral("locator")).toString(),
              specs[0].layoutIdentity->locator);
    EXPECT_EQ(secondJson.value(QStringLiteral("locator")).toString(),
              specs[1].layoutIdentity->locator);
    EXPECT_FALSE(multi.getDisplayName().contains(QStringLiteral("rumble-")));

    auto message = std::make_shared<Message>();
    message->id = QStringLiteral("one");
    message->loginName = QStringLiteral("author");
    MessagePtr immutable = message;
    runtime->messageAppended.invoke(immutable, std::nullopt);
    EXPECT_EQ(multi.countMessages(), 1U);
}

TEST(RumbleWindowDescriptor, SharedRuntimeMultiRetriesOneLifecycleGeneration)
{
    Fixture fixture;
    auto first = fixture.controller.restore(
        QStringLiteral("https://rumble.com/embed/vsame"));
    auto second = fixture.controller.restore(
        QStringLiteral("https://rumble.com/vsame-title.html"));
    ASSERT_EQ(first.get(), second.get());
    const std::array specs{
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = first.layoutIdentity()->locator,
            .layoutIdentity = *first.layoutIdentity(),
        },
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = second.layoutIdentity()->locator,
        },
    };
    auto multi = std::make_shared<MultiChannel>(
        specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [runtime = first.get()](const auto &) {
            return runtime;
        });

    EXPECT_TRUE(fixture.scheduler.callbacks.empty());
    ASSERT_TRUE(multi->channels()[1].spec().layoutIdentity);
    EXPECT_EQ(multi->channels()[1].spec().layoutIdentity->locator,
              second.layoutIdentity()->locator);
    IndirectChannel nested(multi, Channel::Type::Multi);
    EXPECT_FALSE(rumbleLayoutNeedsPickerRepair(nested, &fixture.controller));
    multi->reconnect();
    EXPECT_EQ(fixture.scheduler.callbacks.size(), 1U);
}

TEST(RumbleWindowDescriptor, RumbleOnlyMultiSettlesWithoutHistoryRegistry)
{
    TestApplication application;
    auto runtime = std::make_shared<Channel>(QStringLiteral("rumble-runtime"),
                                             Channel::Type::Rumble);
    const MultiChannel::Spec spec{
        .platform = MultiChannel::Platform::Rumble,
        .name = QStringLiteral("https://rumble.com/embed/vhistory"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator = QStringLiteral("https://rumble.com/embed/vhistory"),
            },
    };
    MultiChannel multi(std::span<const MultiChannel::Spec>(&spec, 1),
                       MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
                       [runtime](const auto &) {
                           return runtime;
                       });

    EXPECT_EQ(messagehistory::Registry::instance().state(runtime),
              messagehistory::State::NotStarted);
    EXPECT_TRUE(multi.initialHistorySettled());
}

TEST(RumbleWindowDescriptor,
     RumbleChildDoesNotBlockTwitchHistoryCommitOrAbortSettlement)
{
    TestApplication application;
    const auto makeMulti = [](const ChannelPtr &twitch,
                              const ChannelPtr &rumble) {
        const std::array specs{
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Twitch,
                .name = QStringLiteral("twitch-history"),
            },
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Rumble,
                .name = QStringLiteral("https://rumble.com/embed/vhistory"),
                .layoutIdentity =
                    ChannelLayoutIdentity{
                        .platform = QStringLiteral("rumble"),
                        .locator =
                            QStringLiteral("https://rumble.com/embed/vhistory"),
                    },
            },
        };
        return std::make_shared<MultiChannel>(
            specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
            [twitch, rumble](const auto &spec) {
                return spec.platform == MultiChannel::Platform::Twitch ? twitch
                                                                       : rumble;
            });
    };

    auto twitch = std::make_shared<Channel>(QStringLiteral("twitch-history"),
                                            Channel::Type::Twitch);
    auto rumble = std::make_shared<Channel>(QStringLiteral("rumble-runtime"),
                                            Channel::Type::Rumble);
    messagehistory::Registry::instance().setLoading(twitch);
    auto committing = makeMulti(twitch, rumble);
    EXPECT_FALSE(committing->initialHistorySettled());

    auto history = std::make_shared<Message>();
    history->messageText = QStringLiteral("twitch history");
    history->flags.set(MessageFlag::RecentMessage);
    MessagePtr immutableHistory = history;
    twitch->addMessagesAtStart({immutableHistory});
    EXPECT_TRUE(committing->getMessageSnapshot().empty());
    messagehistory::Registry::instance().setLoaded(twitch);
    EXPECT_TRUE(committing->initialHistorySettled());
    ASSERT_EQ(committing->getMessageSnapshot().size(), 1U);
    EXPECT_EQ(committing->getMessageSnapshot().front()->messageText,
              QStringLiteral("twitch history"));

    auto failingTwitch = std::make_shared<Channel>(
        QStringLiteral("failing-history"), Channel::Type::Twitch);
    auto secondRumble = std::make_shared<Channel>(
        QStringLiteral("second-rumble-runtime"), Channel::Type::Rumble);
    messagehistory::Registry::instance().setLoading(failingTwitch);
    auto aborting = makeMulti(failingTwitch, secondRumble);
    EXPECT_FALSE(aborting->initialHistorySettled());
    messagehistory::Registry::instance().setFailed(
        failingTwitch, QStringLiteral("bounded failure"));
    EXPECT_TRUE(aborting->initialHistorySettled());
}

TEST(RumbleWindowDescriptor, LowercaseNestedPlatformRestoresTypedPlaceholder)
{
    ChildChannelDescriptor lower = ChildChannelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("platform"), QStringLiteral("rumble")},
        {QStringLiteral("locator"),
         QStringLiteral("https://rumble.com/embed/vsafe")},
    });
    auto spec = MultiChannel::Spec::fromDescriptor(lower);
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->platform, MultiChannel::Platform::Rumble);
    ASSERT_TRUE(spec->layoutIdentity);
    EXPECT_EQ(spec->layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vsafe"));

    auto malformedWithFallback = ChildChannelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("platform"), QStringLiteral("Rumble")},
        {QStringLiteral("channel"), QStringLiteral("rumble-secret-runtime-id")},
        {QStringLiteral("locator"),
         QStringLiteral("https://rumble.com/embed/vlegacy?drop=1")},
        {QStringLiteral("futureOptional"),
         QJsonObject{
             {QStringLiteral("nested"), QJsonArray{1, 2, 3}},
         }},
        {QStringLiteral("layoutIdentity"),
         QJsonObject{
             {QStringLiteral("platform"), QStringLiteral("wrong")},
             {QStringLiteral("locator"), QStringLiteral("secret-sentinel")},
         }},
    });
    auto fallbackSpec =
        MultiChannel::Spec::fromDescriptor(malformedWithFallback);
    ASSERT_TRUE(fallbackSpec);
    ASSERT_TRUE(fallbackSpec->layoutIdentity);
    EXPECT_EQ(fallbackSpec->layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vlegacy"));

    auto malformedWithoutFallback =
        ChildChannelDescriptor::fromJson(QJsonObject{
            {QStringLiteral("platform"), QStringLiteral("Rumble")},
            {QStringLiteral("channel"),
             QStringLiteral("rumble-secret-runtime-id")},
            {QStringLiteral("layoutIdentity"),
             QJsonObject{
                 {QStringLiteral("platform"), QStringLiteral("rumble")},
                 {QStringLiteral("locator"),
                  QStringLiteral("https://example.com/?secret=sentinel")},
             }},
        });
    auto emptySpec =
        MultiChannel::Spec::fromDescriptor(malformedWithoutFallback);
    ASSERT_TRUE(emptySpec);
    ASSERT_TRUE(emptySpec->layoutIdentity);
    EXPECT_TRUE(emptySpec->name.isEmpty());
    EXPECT_TRUE(emptySpec->layoutIdentity->locator.isEmpty());
}

TEST(RumbleWindowDescriptor, TwitchSpecAndDisplayKeepLegacyRuntimeName)
{
    TestApplication application;
    auto channel = std::make_shared<DisplayChannel>(
        QStringLiteral("stable-login"), QStringLiteral("Pretty Name"));
    const MultiChannel::Spec spec{
        .platform = MultiChannel::Platform::Twitch,
        .name = QStringLiteral("construction-input"),
    };
    MultiChannel multi(std::span<const MultiChannel::Spec>(&spec, 1),
                       MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
                       [channel](const auto &) {
                           return channel;
                       });
    EXPECT_EQ(multi.channels().front().spec().name,
              QStringLiteral("stable-login"));
    EXPECT_EQ(multi.getDisplayName(), QStringLiteral("stable-login"));
}

TEST(RumbleWindowDescriptor, LegacyPlatformsAndStreamValuesRemainStable)
{
    EXPECT_EQ(static_cast<int>(MultiChannel::Platform::Twitch), 0);
    EXPECT_EQ(static_cast<int>(MultiChannel::Platform::Kick), 1);
    EXPECT_EQ(static_cast<int>(MultiChannel::Platform::Rumble), 2);

    const MultiChannel::Spec rumble{
        .platform = MultiChannel::Platform::Rumble,
        .name = QStringLiteral("https://rumble.com/embed/vsafe"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator = QStringLiteral("https://rumble.com/embed/vsafe"),
            },
    };
    QByteArray rumbleBytes;
    {
        QDataStream stream(&rumbleBytes, QIODevice::WriteOnly);
        stream << rumble;
    }
    MultiChannel::Spec reused;
    {
        QDataStream stream(&rumbleBytes, QIODevice::ReadOnly);
        stream >> reused;
    }
    ASSERT_TRUE(reused.layoutIdentity);

    const ChildChannelDescriptor directDescriptor{
        .platform = QStringLiteral("Rumble"),
        .channelName = QStringLiteral("12345"),
    };
    auto directSpec = MultiChannel::Spec::fromDescriptor(directDescriptor);
    ASSERT_TRUE(directSpec);
    EXPECT_TRUE(directSpec->name.isEmpty());
    ASSERT_TRUE(directSpec->layoutIdentity);
    EXPECT_TRUE(directSpec->layoutIdentity->locator.isEmpty());

    const ChildChannelDescriptor legacyPublicDescriptor{
        .platform = QStringLiteral("Rumble"),
        .channelName =
            QStringLiteral("https://rumble.com/embed/vlegacy?drop=1"),
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("wrong"),
                .locator = QStringLiteral("secret-sentinel"),
            },
    };
    auto legacyPublicSpec =
        MultiChannel::Spec::fromDescriptor(legacyPublicDescriptor);
    ASSERT_TRUE(legacyPublicSpec);
    EXPECT_EQ(legacyPublicSpec->name,
              QStringLiteral("https://rumble.com/embed/vlegacy"));
    ASSERT_TRUE(legacyPublicSpec->layoutIdentity);
    EXPECT_EQ(legacyPublicSpec->layoutIdentity->locator,
              legacyPublicSpec->name);

    auto typedMalformedWithFallback = legacyPublicDescriptor;
    typedMalformedWithFallback.layoutIdentity->platform =
        QStringLiteral("rumble");
    typedMalformedWithFallback.layoutIdentity->locator =
        QStringLiteral("https://example.com/?secret=sentinel");
    auto typedFallbackSpec =
        MultiChannel::Spec::fromDescriptor(typedMalformedWithFallback);
    ASSERT_TRUE(typedFallbackSpec);
    EXPECT_EQ(typedFallbackSpec->name,
              QStringLiteral("https://rumble.com/embed/vlegacy"));

    const MultiChannel::Spec nameOnly{
        .platform = MultiChannel::Platform::Rumble,
        .name =
            QStringLiteral("https://rumble.com/embed/vnameonly?secret=drop"),
    };
    const auto nameOnlyJson = nameOnly.descriptor().toJson();
    EXPECT_EQ(nameOnlyJson.value(QStringLiteral("locator")).toString(),
              QStringLiteral("https://rumble.com/embed/vnameonly"));
    EXPECT_FALSE(QJsonDocument(nameOnlyJson).toJson().contains("secret=drop"));

    QByteArray craftedDirectBytes;
    {
        QDataStream stream(&craftedDirectBytes, QIODevice::WriteOnly);
        stream << MultiChannel::Platform::Rumble << QStringLiteral("12345");
    }
    {
        QDataStream stream(&craftedDirectBytes, QIODevice::ReadOnly);
        stream >> reused;
    }
    EXPECT_EQ(reused.platform, MultiChannel::Platform::Rumble);
    EXPECT_TRUE(reused.name.isEmpty());
    ASSERT_TRUE(reused.layoutIdentity);
    EXPECT_TRUE(reused.layoutIdentity->locator.isEmpty());

    const MultiChannel::Spec kick{
        .platform = MultiChannel::Platform::Kick,
        .name = QStringLiteral("stable-kick-slug"),
    };
    QByteArray kickBytes;
    {
        QDataStream stream(&kickBytes, QIODevice::WriteOnly);
        stream << kick;
    }
    {
        QDataStream stream(&kickBytes, QIODevice::ReadOnly);
        stream >> reused;
    }
    EXPECT_EQ(reused.platform, MultiChannel::Platform::Kick);
    EXPECT_EQ(reused.name, QStringLiteral("stable-kick-slug"));
    EXPECT_FALSE(reused.layoutIdentity);

    SplitDescriptor kickDescriptor;
    SplitDescriptor::loadFromJSON(
        kickDescriptor, QJsonObject{},
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("kick")},
            {QStringLiteral("name"), QStringLiteral("stable-kick-slug")},
            {QStringLiteral("channelID"), 101},
            {QStringLiteral("userID"), 202},
            {QStringLiteral("roomID"), 303},
        });
    EXPECT_EQ(kickDescriptor.channelName_, QStringLiteral("stable-kick-slug"));
    EXPECT_EQ(kickDescriptor.kickChannelID, 101U);
    EXPECT_EQ(kickDescriptor.kickUserID, 202U);
    EXPECT_EQ(kickDescriptor.kickRoomID, 303U);

    ChildChannelDescriptor twitch{
        .platform = QStringLiteral("Twitch"),
        .channelName = QStringLiteral("stable-twitch-login"),
    };
    ChildChannelDescriptor kickChild{
        .platform = QStringLiteral("Kick"),
        .channelName = QStringLiteral("stable-kick-slug"),
    };
    EXPECT_EQ(ChildChannelDescriptor::fromJson(twitch.toJson()).channelName,
              twitch.channelName);
    EXPECT_EQ(ChildChannelDescriptor::fromJson(kickChild.toJson()).channelName,
              kickChild.channelName);
}
