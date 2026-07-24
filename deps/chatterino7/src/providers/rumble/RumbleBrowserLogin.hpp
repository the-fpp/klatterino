// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QObject;
class QJsonObject;
class QRect;

namespace chatterino::rumble {

enum class BrowserLoginOutcome : std::uint8_t {
    Session,
    Cancelled,
    Unavailable,
    Failed,
};

enum class BrowserLoginReadiness : std::uint8_t {
    Idle,
    Preparing,
    Ready,
    Unavailable,
    Failed,
};

enum class BrowserConsentState : std::uint8_t {
    Pending,
    Resolved,
    Rejected,
    RumblePage,
    ExternalPage,
};

struct BrowserLoginStatus {
    BrowserLoginReadiness readiness = BrowserLoginReadiness::Idle;
    QString browserName;
    QString userMessage;
};

struct SeleniumManagerResult {
    QString driverPath;
    QString browserPath;
};

struct BrowserLoginResult {
    BrowserLoginOutcome outcome = BrowserLoginOutcome::Failed;
    QByteArray session;
    QString accountName;
    QString userMessage;
};

struct BrowserWindowGeometry {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

/// Parses the helper's single bounded, closed-vocabulary stdout record.
/// Successful session bytes must be moved immediately into SessionController.
BrowserLoginResult parseBrowserLoginOutput(QByteArray output);
std::optional<QString> parseBrowserAccountName(const QByteArray &output);
/// Parses the bounded result of the read-only Ketch consent/navigation monitor.
std::optional<BrowserConsentState> parseBrowserConsentState(
    const QByteArray &output);
/// Returns the page-local consent guard. Until Ketch reports a user choice,
/// Rumble's own page roots are inert while Ketch's roots remain untouched.
QString browserConsentMonitorScript();
std::optional<SeleniumManagerResult> parseSeleniumManagerOutput(
    const QByteArray &output);
/// Fits the portrait login surface within Qt's logical available-display
/// coordinates, converting once to the device pixels used by the isolated
/// scale-factor-1 browser and WebDriver outer-window rect.
BrowserWindowGeometry fitPortraitBrowserWindow(const QRect &availableGeometry,
                                               double devicePixelRatio);
/// Returns the arguments used to launch a Chromium-family browser before
/// ChromeDriver attaches to it. Keeping browser launch ownership here avoids
/// WebDriver's automation extension while preserving a dedicated app window.
QStringList chromiumBrowserArguments(const QString &profile,
                                     std::uint16_t debuggingPort,
                                     const BrowserWindowGeometry &geometry,
                                     bool forceX11);
/// Firefox has no supported Chromium-style app-window mode. Its isolated
/// fallback remains fitted and movable without requesting kiosk/fullscreen.
QStringList firefoxBrowserArguments(const BrowserWindowGeometry &geometry);
/// Returns the sole ChromeOptions capability used to attach ChromeDriver to
/// the already-running temporary browser.
QJsonObject chromiumAttachOptions(std::uint16_t debuggingPort);
/// Owns one short-lived system-browser/WebDriver login. Browser and driver stay
/// out of process and must exit before a session result is accepted.
class BrowserLogin final
{
public:
    using Callback = std::function<void(BrowserLoginResult)>;
    using PrepareCallback = std::function<void(BrowserLoginStatus)>;

    explicit BrowserLogin(QObject *owner);
    ~BrowserLogin();
    BrowserLogin(const BrowserLogin &) = delete;
    BrowserLogin &operator=(const BrowserLogin &) = delete;

    void prepare(PrepareCallback callback);
    bool start(Callback callback);
    void cancel() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] BrowserLoginStatus status() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace chatterino::rumble
