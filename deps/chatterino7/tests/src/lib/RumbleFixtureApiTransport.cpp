// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureApiTransport.hpp"

#include <QUrl>

#include <utility>

namespace chatterino::test {
namespace {

class ApiFixtureHandle final : public rumble::TransportHandle
{
public:
    explicit ApiFixtureHandle(RumbleFixtureRequestHandle handle)
        : handle_(std::move(handle))
    {
    }

    void cancel() noexcept override
    {
        this->handle_.cancel();
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return this->handle_.active();
    }

private:
    RumbleFixtureRequestHandle handle_;
};

}  // namespace

RumbleFixtureApiTransport::RumbleFixtureApiTransport(
    RumbleFixtureTransport &transport)
    : transport_(transport)
{
}

std::unique_ptr<rumble::TransportHandle> RumbleFixtureApiTransport::start(
    rumble::TransportRequest request, rumble::TransportCallbacks callbacks)
{
    auto target = request.url.path(QUrl::FullyEncoded);
    if (request.url.hasQuery())
        target += QStringLiteral("?") + request.url.query(QUrl::FullyEncoded);

    RumbleFixtureRequest fixtureRequest{
        .method = "GET",
        .target = target.toStdString(),
    };
    for (const auto &header : request.headers)
    {
        fixtureRequest.headers.push_back(
            {header.name.toStdString(), header.value.toStdString()});
    }

    RumbleFixtureCallbacks converted;
    converted.onHead = [callback = std::move(callbacks.onHead)](
                           const RumbleFixtureResponseHead &head) {
        if (!callback)
            return;
        rumble::ResponseHead result{.status = head.status};
        for (const auto &header : head.headers)
        {
            result.headers.push_back({QByteArray::fromStdString(header.name),
                                      QByteArray::fromStdString(header.value)});
        }
        callback(result);
    };
    converted.onBodyChunk = [callback = std::move(callbacks.onBodyChunk)](
                                const std::string &chunk) {
        if (callback)
            callback(QByteArray::fromStdString(chunk));
    };
    converted.onComplete = std::move(callbacks.onComplete);
    converted.onDisconnect =
        [callback = std::move(callbacks.onFailure)](const std::string &) {
            if (callback)
                callback(rumble::TransportFailure::Network);
        };

    return std::make_unique<ApiFixtureHandle>(this->transport_.start(
        std::move(fixtureRequest), std::move(converted)));
}

}  // namespace chatterino::test
