// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "providers/rumble/RumbleBrowserLogin.hpp"
#include "providers/rumble/RumbleLayoutLocator.hpp"
#include "util/Expected.hpp"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class QObject;

namespace chatterino {

class RumbleChannel;
class RumbleDispatcher;
class RumbleAccountManager;

namespace rumble {
class RumbleApi;
class RumbleScheduler;
enum class SessionState : std::uint8_t;
}  // namespace rumble

enum class RumbleLayoutResolveErrorCode : std::uint8_t {
    InvalidLocator,
    DirectStreamNotPersistable,
    ProviderUnavailable,
    ResolutionFailed,
};

struct RumbleLayoutResolveError {
    RumbleLayoutResolveErrorCode code =
        RumbleLayoutResolveErrorCode::ResolutionFailed;
    QString userMessage;

    friend bool operator==(const RumbleLayoutResolveError &,
                           const RumbleLayoutResolveError &) = default;
};

/// Visible typed placeholder used when a saved locator cannot currently be
/// attached to a provider. A validated locator is retained; unsafe input is
/// represented by an empty locator and must be repaired through the picker.
IndirectChannel makeRumbleLayoutPlaceholder(QString canonicalLocator = {});

/// Application-owned bridge between stable layout locators and #22's bounded
/// connection lifecycle.  One lifecycle is retained per runtime channel while
/// every returned IndirectChannel owns an independent public layout identity.
class RumbleApplicationController final
{
public:
    using ResolveResult = Expected<IndirectChannel, RumbleLayoutResolveError>;
    using ResolveCallback = std::function<void(ResolveResult)>;
    using ChannelFactory = std::function<std::shared_ptr<RumbleChannel>(
        const RumbleLayoutLocator &)>;

    class Request final
    {
    public:
        Request() = default;
        Request(const Request &) = delete;
        Request &operator=(const Request &) = delete;
        Request(Request &&other) noexcept;
        Request &operator=(Request &&other) noexcept;
        ~Request();

        void cancel() noexcept;
        [[nodiscard]] bool active() const noexcept;

    private:
        struct State;
        explicit Request(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;

        friend class RumbleApplicationController;
    };

    /// Production constructor. All owned Qt objects use affinityOwner's
    /// thread; when null, a private owner is created on the current thread.
    explicit RumbleApplicationController(
        QObject *affinityOwner = nullptr,
        RumbleAccountManager *accountManager = nullptr);

    /// Deterministic seam for tests. Dependencies and any channels returned by
    /// channelFactory must outlive this controller.
    RumbleApplicationController(rumble::RumbleApi &api,
                                rumble::RumbleScheduler &scheduler,
                                std::shared_ptr<RumbleDispatcher> dispatcher,
                                ChannelFactory channelFactory = {});

    ~RumbleApplicationController();

    RumbleApplicationController(const RumbleApplicationController &) = delete;
    RumbleApplicationController &operator=(
        const RumbleApplicationController &) = delete;

    /// Starts or shares a bounded lifecycle and returns immediately with a
    /// visible Rumble-typed channel. Invalid persisted data yields a visible
    /// sanitized placeholder and is never retained verbatim.
    IndirectChannel restore(const QString &savedLocator);

    /// Picker path. Validation happens before any provider/network work and
    /// completion is never delivered after Request::cancel().
    [[nodiscard]] Request resolve(const QString &userInput,
                                  ResolveCallback callback);

    /// Retry using this view's retained public locator rather than the runtime
    /// channel's opaque identity or another view's locator.
    void retry(const IndirectChannel &channel);
    void retry(const QString &canonicalLocator);

    /// Phase one closes every subscriber gate and cancels connection work but
    /// leaves channels/locator metadata alive for layout serialization.
    void beginShutdown() noexcept;
    /// Final phase closes provider channels. Both phases are idempotent.
    void shutdown() noexcept;

    [[nodiscard]] bool isShuttingDown() const noexcept;

    /// Test/compatibility seam for an explicit memory-only bearer import. The
    /// production account flow persists only through RumbleAccountManager's
    /// OS credential store.
    bool importSession(QByteArray bearer);
    /// Starts a short-lived system-browser login, validates it after browser
    /// exit, and then asks RumbleAccountManager to store the account securely.
    void prepareBrowserLogin(
        std::function<void(rumble::BrowserLoginStatus)> callback);
    [[nodiscard]] rumble::BrowserLoginStatus browserLoginStatus() const;
    void loginInBrowser(std::function<void(bool, QString)> callback);
    void cancelBrowserLogin() noexcept;
    [[nodiscard]] bool browserLoginActive() const noexcept;
    void validateSession(std::function<void(bool, QString)> callback);
    void clearSession() noexcept;
    [[nodiscard]] rumble::SessionState sessionState() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

/// Returns true when reconnect cannot safely target a retained public locator
/// and the channel picker must be opened instead. Multi-channel children are
/// checked independently so an unavailable controller never advertises a
/// retry action that can only reach a placeholder no-op.
bool rumbleLayoutNeedsPickerRepair(
    const IndirectChannel &channel,
    const RumbleApplicationController *controller);

}  // namespace chatterino
