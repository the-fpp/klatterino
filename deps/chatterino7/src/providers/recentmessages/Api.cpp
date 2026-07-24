// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Api.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/history/MessageHistoryLoadRegistry.hpp"
#include "providers/recentmessages/Impl.hpp"
#include "util/PostToThread.hpp"

#include <QTimer>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoRecentMessages;
constexpr int MAX_HISTORY_LOAD_ATTEMPTS = 3;
constexpr int HISTORY_RETRY_DELAY_MS = 500;

}  // namespace

namespace chatterino::recentmessages {

using namespace recentmessages::detail;

namespace {

bool isInitialLoad(
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        &after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        &before)
{
    return !after.has_value() && !before.has_value();
}

void loadAttempt(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before,
    bool jitter, int attempt);

void finishFailure(std::weak_ptr<Channel> channelPtr, ErrorCallback onError,
                   QString error, bool trackInitialLoad)
{
    auto shared = channelPtr.lock();
    if (!shared)
    {
        return;
    }

    postToThread([shared = std::move(shared), onError = std::move(onError),
                  error = std::move(error), trackInitialLoad]() mutable {
        if (isAppAboutToQuit())
        {
            return;
        }

        shared->addSystemMessage(
            QStringLiteral("Couldn't load message history. Try again later."));
        onError();
        if (trackInitialLoad)
        {
            messagehistory::Registry::instance().setFailed(shared,
                                                           std::move(error));
        }
    });
}

void retryOrFail(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before,
    bool jitter, int attempt, QString error)
{
    if (attempt < MAX_HISTORY_LOAD_ATTEMPTS)
    {
        QTimer::singleShot(
            HISTORY_RETRY_DELAY_MS * attempt,
            [channelName, channelPtr, onLoaded = std::move(onLoaded),
             onError = std::move(onError), limit, after, before, jitter,
             attempt]() mutable {
                loadAttempt(channelName, channelPtr, std::move(onLoaded),
                            std::move(onError), limit, after, before, jitter,
                            attempt + 1);
            });
        return;
    }

    finishFailure(channelPtr, std::move(onError), std::move(error),
                  isInitialLoad(after, before));
}

void loadAttempt(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before,
    const bool jitter, const int attempt)
{
    qCDebug(LOG) << "Loading recent messages for" << channelName << "attempt"
                 << attempt;

    const auto url =
        constructRecentMessagesUrl(channelName, limit, after, before);
    const long delayMs = jitter ? std::rand() % 100 : 0;

    QTimer::singleShot(delayMs, [=] {
        if (isAppAboutToQuit())
        {
            return;
        }

        NetworkRequest(url)
            .onSuccess([=](const NetworkResult &result) mutable {
                assert(!isAppAboutToQuit());

                auto shared = channelPtr.lock();
                if (!shared)
                {
                    return;
                }

                auto root = result.parseJson();
                const auto errorCode = root.value("error_code").toString();
                if (!errorCode.isEmpty())
                {
                    qCDebug(LOG) << "Recent-message API returned error_code"
                                 << errorCode << "for" << shared->getName();
                    retryOrFail(
                        channelName, channelPtr, std::move(onLoaded),
                        std::move(onError), limit, after, before, jitter, attempt,
                        QStringLiteral("remote service error"));
                    return;
                }

                qCDebug(LOG) << "Successfully loaded recent messages for"
                             << shared->getName();

                auto parsedMessages = parseRecentMessages(root);
                auto builtMessages =
                    buildRecentMessages(parsedMessages, shared.get());
                const bool trackInitialLoad = isInitialLoad(after, before);

                postToThread(
                    [shared = std::move(shared),
                     messages = std::move(builtMessages),
                     onLoaded = std::move(onLoaded), trackInitialLoad]() mutable {
                        assert(!isAppAboutToQuit());
                        onLoaded(messages);
                        if (trackInitialLoad)
                        {
                            messagehistory::Registry::instance().setLoaded(
                                shared);
                        }
                    });
            })
            .onError([=](const NetworkResult &result) mutable {
                auto shared = channelPtr.lock();
                if (!shared)
                {
                    return;
                }
                assert(!isAppAboutToQuit());

                const auto error = result.formatError();
                qCDebug(LOG) << "Failed to load recent messages for"
                             << shared->getName() << "attempt" << attempt << ':'
                             << error;
                retryOrFail(channelName, channelPtr, std::move(onLoaded),
                            std::move(onError), limit, after, before, jitter,
                            attempt, error);
            })
            .execute();
    });
}

}  // namespace

void load(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before,
    const bool jitter)
{
    if (isInitialLoad(after, before))
    {
        if (auto channel = channelPtr.lock())
        {
            messagehistory::Registry::instance().setLoading(channel);
        }
    }

    loadAttempt(channelName, std::move(channelPtr), std::move(onLoaded),
                std::move(onError), limit, after, before, jitter, 1);
}

}  // namespace chatterino::recentmessages
