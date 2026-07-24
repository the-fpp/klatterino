// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleBrowserLogin.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QScreen>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace chatterino::rumble {
namespace {

constexpr qsizetype MAX_OUTPUT_BYTES = 8 * 1024;
constexpr qsizetype MAX_MANAGER_OUTPUT_BYTES = 256 * 1024;
constexpr qsizetype MAX_DRIVER_RESPONSE_BYTES = 256 * 1024;
constexpr auto MANAGER_TIMEOUT_MS = 5 * 60 * 1000;
constexpr auto LOGIN_URL = "https://rumble.com/account/login";

enum class BrowserKind : std::uint8_t {
    Firefox,
    Chrome,
    Chromium,
    Edge,
    Brave,
};

struct BrowserDescriptor {
    BrowserKind kind = BrowserKind::Firefox;
    QString displayName;
    QString managerName;
    QString executable;
};

void wipe(QByteArray &value) noexcept
{
    volatile char *bytes = value.data();
    for (qsizetype i = 0; i < value.size(); ++i)
    {
        bytes[i] = 0;
    }
    value.clear();
    value.squeeze();
}

bool safeSession(const QByteArray &value)
{
    return !value.isEmpty() && value.size() <= 4096 &&
           std::ranges::none_of(value, [](unsigned char ch) {
               return ch <= 0x20 || ch == 0x7f || ch == ';' || ch == ',';
           });
}

BrowserLoginResult result(BrowserLoginOutcome outcome, QString message)
{
    return {
        .outcome = outcome,
        .userMessage = std::move(message),
    };
}

BrowserLoginResult failed()
{
    return result(
        BrowserLoginOutcome::Failed,
        QStringLiteral("Rumble sign-in couldn't be completed. Try again."));
}

BrowserLoginResult cancelled()
{
    return result(BrowserLoginOutcome::Cancelled,
                  QStringLiteral("Rumble sign-in was cancelled."));
}

BrowserLoginResult unavailable(QString message = {})
{
    return result(
        BrowserLoginOutcome::Unavailable,
        message.isEmpty()
            ? QStringLiteral("No supported browser is ready for Rumble sign-in.")
            : std::move(message));
}

QString firstExecutable(std::initializer_list<QString> candidates)
{
    for (const auto &candidate : candidates)
    {
        const auto executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty())
        {
            return QFileInfo(executable).canonicalFilePath();
        }
    }
    return {};
}

bool isExecutableFile(const QString &path)
{
    if (path.isEmpty())
    {
        return false;
    }
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

QString firstFile(const QStringList &candidates)
{
    for (const auto &candidate : candidates)
    {
        QFileInfo info(candidate);
        if (isExecutableFile(candidate))
        {
            const auto canonical = info.canonicalFilePath();
            return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
        }
    }
    return {};
}

std::optional<BrowserDescriptor> discoverBrowser()
{
#ifdef Q_OS_WIN
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    const auto programFiles = qEnvironmentVariable("ProgramFiles");
    const auto programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
    const std::array candidates{
        BrowserDescriptor{
            BrowserKind::Edge,
            QStringLiteral("Microsoft Edge"),
            QStringLiteral("edge"),
            firstFile(
                {programFilesX86 +
                     QStringLiteral("/Microsoft/Edge/Application/msedge.exe"),
                 programFiles +
                     QStringLiteral("/Microsoft/Edge/Application/msedge.exe"),
                 QStandardPaths::findExecutable(QStringLiteral("msedge.exe"))}),
        },
        BrowserDescriptor{
            BrowserKind::Chrome,
            QStringLiteral("Google Chrome"),
            QStringLiteral("chrome"),
            firstFile(
                {programFiles +
                     QStringLiteral("/Google/Chrome/Application/chrome.exe"),
                 programFilesX86 +
                     QStringLiteral("/Google/Chrome/Application/chrome.exe"),
                 local +
                     QStringLiteral("/Google/Chrome/Application/chrome.exe"),
                 QStandardPaths::findExecutable(QStringLiteral("chrome.exe"))}),
        },
        BrowserDescriptor{
            BrowserKind::Brave,
            QStringLiteral("Brave"),
            QStringLiteral("chrome"),
            firstFile(
                {programFiles +
                     QStringLiteral("/BraveSoftware/Brave-Browser/Application/"
                                    "brave.exe"),
                 local +
                     QStringLiteral("/BraveSoftware/Brave-Browser/Application/"
                                    "brave.exe"),
                 QStandardPaths::findExecutable(QStringLiteral("brave.exe"))}),
        },
        BrowserDescriptor{
            BrowserKind::Firefox,
            QStringLiteral("Mozilla Firefox"),
            QStringLiteral("firefox"),
            firstFile(
                {programFiles + QStringLiteral("/Mozilla Firefox/firefox.exe"),
                 programFilesX86 +
                     QStringLiteral("/Mozilla Firefox/firefox.exe"),
                 QStandardPaths::findExecutable(
                     QStringLiteral("firefox.exe"))}),
        },
    };
#elif defined(Q_OS_MACOS)
    const std::array candidates{
        BrowserDescriptor{
            BrowserKind::Chrome,
            QStringLiteral("Google Chrome"),
            QStringLiteral("chrome"),
            firstFile({QStringLiteral(
                "/Applications/Google Chrome.app/Contents/MacOS/"
                "Google Chrome")}),
        },
        BrowserDescriptor{
            BrowserKind::Edge,
            QStringLiteral("Microsoft Edge"),
            QStringLiteral("edge"),
            firstFile({QStringLiteral(
                "/Applications/Microsoft Edge.app/Contents/MacOS/"
                "Microsoft Edge")}),
        },
        BrowserDescriptor{
            BrowserKind::Brave,
            QStringLiteral("Brave"),
            QStringLiteral("chrome"),
            firstFile({QStringLiteral(
                "/Applications/Brave Browser.app/Contents/MacOS/"
                "Brave Browser")}),
        },
        BrowserDescriptor{
            BrowserKind::Chromium,
            QStringLiteral("Chromium"),
            QStringLiteral("chrome"),
            firstFile({QStringLiteral(
                "/Applications/Chromium.app/Contents/MacOS/Chromium")}),
        },
        BrowserDescriptor{
            BrowserKind::Firefox,
            QStringLiteral("Mozilla Firefox"),
            QStringLiteral("firefox"),
            firstFile({QStringLiteral(
                "/Applications/Firefox.app/Contents/MacOS/firefox")}),
        },
    };
#else
    const std::array candidates{
        BrowserDescriptor{
            BrowserKind::Chrome,
            QStringLiteral("Google Chrome"),
            QStringLiteral("chrome"),
            firstExecutable({QStringLiteral("google-chrome-stable"),
                             QStringLiteral("google-chrome"),
                             QStringLiteral("chrome")}),
        },
        BrowserDescriptor{
            BrowserKind::Chromium,
            QStringLiteral("Chromium"),
            QStringLiteral("chrome"),
            firstExecutable({QStringLiteral("chromium"),
                             QStringLiteral("chromium-browser")}),
        },
        BrowserDescriptor{
            BrowserKind::Edge,
            QStringLiteral("Microsoft Edge"),
            QStringLiteral("edge"),
            firstExecutable({QStringLiteral("microsoft-edge-stable"),
                             QStringLiteral("microsoft-edge")}),
        },
        BrowserDescriptor{
            BrowserKind::Brave,
            QStringLiteral("Brave"),
            QStringLiteral("chrome"),
            firstExecutable(
                {QStringLiteral("brave-browser"), QStringLiteral("brave")}),
        },
        BrowserDescriptor{
            BrowserKind::Firefox,
            QStringLiteral("Mozilla Firefox"),
            QStringLiteral("firefox"),
            firstExecutable(
                {QStringLiteral("firefox"), QStringLiteral("firefox-esr")}),
        },
    };
#endif
    for (const auto &candidate : candidates)
    {
        if (!candidate.executable.isEmpty())
        {
            return candidate;
        }
    }
    return std::nullopt;
}

QString discoverSeleniumManager()
{
    auto path = qEnvironmentVariable("CHATTERINO_SELENIUM_MANAGER");
    if (isExecutableFile(path))
    {
        return QFileInfo(path).absoluteFilePath();
    }
#ifdef Q_OS_WIN
    constexpr auto executable = "selenium-manager.exe";
#else
    constexpr auto executable = "selenium-manager";
#endif
    path = QCoreApplication::applicationDirPath() + QLatin1Char('/') +
           QString::fromLatin1(executable);
    if (isExecutableFile(path))
    {
        return QFileInfo(path).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QString::fromLatin1(executable));
}

std::optional<QByteArray> sessionCookie(const QJsonArray &cookies)
{
    std::vector<QByteArray> matches;
    for (const auto &value : cookies)
    {
        const auto cookie = value.toObject();
        if (cookie.value(QStringLiteral("name")).toString() !=
            QStringLiteral("u_s"))
        {
            continue;
        }
        auto domain =
            cookie.value(QStringLiteral("domain")).toString().toLower();
        if (domain.startsWith(QLatin1Char('.')))
        {
            domain.remove(0, 1);
        }
        if (domain != QStringLiteral("rumble.com") &&
            domain != QStringLiteral("www.rumble.com"))
        {
            continue;
        }
        if (cookie.value(QStringLiteral("path")).toString() !=
            QStringLiteral("/"))
        {
            continue;
        }
        const auto candidate =
            cookie.value(QStringLiteral("value")).toString().toUtf8();
        if (safeSession(candidate) &&
            std::ranges::find(matches, candidate) == matches.end())
        {
            matches.push_back(candidate);
        }
    }
    if (matches.size() != 1)
    {
        return std::nullopt;
    }
    return std::move(matches.front());
}

QString cookieReadFailure(int status, const QByteArray &body)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    const auto error = document.object()
                           .value(QStringLiteral("value"))
                           .toObject()
                           .value(QStringLiteral("error"))
                           .toString();
    if (error == QStringLiteral("invalid session id") ||
        error == QStringLiteral("no such window"))
    {
        return QStringLiteral(
            "The sign-in window closed before Rumble sign-in finished. Try "
            "again.");
    }
    if (error == QStringLiteral("timeout"))
    {
        return QStringLiteral("Rumble sign-in took too long. Try again.");
    }
    if (error == QStringLiteral("unexpected alert open"))
    {
        return QStringLiteral(
            "Close the browser alert before continuing Rumble sign-in.");
    }
    if (status == 0)
    {
        return QStringLiteral("Rumble sign-in could not continue. Try again.");
    }
    return QStringLiteral("Rumble sign-in could not be verified. Try again.");
}

bool supportedChromiumProduct(const QString &product)
{
    static const std::array prefixes{
        QStringLiteral("Chrome/"),
        QStringLiteral("Chromium/"),
        QStringLiteral("Edg/"),
        QStringLiteral("Microsoft Edge/"),
    };
    const auto prefix =
        std::ranges::find_if(prefixes, [&product](const auto &candidate) {
            return product.startsWith(candidate);
        });
    if (prefix == prefixes.end())
    {
        return false;
    }
    const auto version = product.sliced(prefix->size());
    return !version.isEmpty() && version.size() <= 32 &&
           version.front().isDigit() && version.back().isDigit() &&
           std::ranges::all_of(version, [](QChar character) {
               return character.isDigit() || character == u'.';
           });
}

}  // namespace

BrowserLoginResult parseBrowserLoginOutput(QByteArray output)
{
    if (output.isEmpty() || output.size() > MAX_OUTPUT_BYTES ||
        output.contains('\0') || output.contains('\r') ||
        !output.endsWith('\n') || output.count('\n') != 1)
    {
        wipe(output);
        return failed();
    }
    output.chop(1);
    if (output == QByteArrayLiteral("RUMBLE_LOGIN_V1 CANCELLED"))
    {
        wipe(output);
        return cancelled();
    }
    if (output == QByteArrayLiteral("RUMBLE_LOGIN_V1 TIMEOUT"))
    {
        wipe(output);
        return result(BrowserLoginOutcome::Failed,
                      QStringLiteral("Rumble sign-in took too long. Try again."));
    }
    if (output == QByteArrayLiteral("RUMBLE_LOGIN_V1 BROWSER_START_FAILED"))
    {
        wipe(output);
        return unavailable(QStringLiteral(
            "Could not open the Rumble sign-in window. Try again."));
    }
    if (output == QByteArrayLiteral("RUMBLE_LOGIN_V1 NAVIGATION_FAILED") ||
        output == QByteArrayLiteral("RUMBLE_LOGIN_V1 FAILED"))
    {
        wipe(output);
        return failed();
    }

    const auto successPrefix = QByteArrayLiteral("RUMBLE_LOGIN_V1 OK ");
    if (!output.startsWith(successPrefix))
    {
        wipe(output);
        return failed();
    }
    auto encoded = output.sliced(successPrefix.size());
    wipe(output);
    auto decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    wipe(encoded);
    if (!decoded || !safeSession(*decoded))
    {
        return failed();
    }
    return {
        .outcome = BrowserLoginOutcome::Session,
        .session = std::move(*decoded),
        .userMessage =
            QStringLiteral("Rumble sign-in completed; confirming the account…"),
    };
}

std::optional<QString> parseBrowserAccountName(const QByteArray &output)
{
    if (output.isEmpty() || output.size() > MAX_DRIVER_RESPONSE_BYTES ||
        output.contains('\0'))
    {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const auto value = document.object().value(QStringLiteral("value"));
    if (!value.isString())
    {
        return std::nullopt;
    }
    auto accountName = value.toString();
    if (accountName.isEmpty() || accountName.size() > 256 ||
        std::ranges::any_of(accountName, [](QChar ch) {
            return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
        }))
    {
        return std::nullopt;
    }
    return accountName;
}

std::optional<BrowserConsentState> parseBrowserConsentState(
    const QByteArray &output)
{
    if (output.isEmpty() || output.size() > MAX_DRIVER_RESPONSE_BYTES ||
        output.contains('\0'))
    {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const auto value =
        document.object().value(QStringLiteral("value")).toString();
    if (value == QStringLiteral("pending"))
    {
        return BrowserConsentState::Pending;
    }
    if (value == QStringLiteral("resolved"))
    {
        return BrowserConsentState::Resolved;
    }
    if (value == QStringLiteral("rejected"))
    {
        return BrowserConsentState::Rejected;
    }
    if (value == QStringLiteral("rumble"))
    {
        return BrowserConsentState::RumblePage;
    }
    if (value == QStringLiteral("external"))
    {
        return BrowserConsentState::ExternalPage;
    }
    return std::nullopt;
}

QString browserConsentMonitorScript()
{
    return QStringLiteral(R"JS(
if (location.protocol === 'https:' &&
    (location.hostname === 'rumble.com' ||
     location.hostname === 'www.rumble.com')) {
    return 'rumble';
}
if (location.protocol !== 'https:' ||
    location.hostname !== 'auth.rumble.com') {
    return 'external';
}

const key = '__chatterinoRumbleConsentMonitorV2';
if (!Object.prototype.hasOwnProperty.call(window, key)) {
    const state = {
        value: 'pending',
        hooked: false,
        readingConsent: false,
        observer: null,
        locked: new Map(),
    };

    const restoreAttribute = (element, name, value) => {
        if (value === null) {
            element.removeAttribute(name);
        } else {
            element.setAttribute(name, value);
        }
    };
    const restoreStyle = (element, name, value) => {
        if (value.value === '') {
            element.style.removeProperty(name);
        } else {
            element.style.setProperty(name, value.value, value.priority);
        }
    };
    const readStyle = (element, name) => ({
        value: element.style.getPropertyValue(name),
        priority: element.style.getPropertyPriority(name),
    });
    const isKetchRoot = element =>
        element.id === 'lanyard_root' ||
        element.id === 'lanyard_fab_button';
    const ignoredTag = element =>
        ['SCRIPT', 'STYLE', 'LINK', 'TEMPLATE', 'NOSCRIPT'].includes(
            element.tagName);

    state.disableRumble = () => {
        if (state.value !== 'pending' || !document.body) {
            return;
        }
        const active = document.activeElement;
        if (active instanceof HTMLElement &&
            !active.closest('#lanyard_root, #lanyard_fab_button')) {
            active.blur();
        }
        for (const element of document.body.children) {
            if (!(element instanceof HTMLElement) ||
                isKetchRoot(element) || ignoredTag(element)) {
                continue;
            }
            if (!state.locked.has(element)) {
                state.locked.set(element, {
                    inert: element.getAttribute('inert'),
                    ariaDisabled: element.getAttribute('aria-disabled'),
                    marker: element.getAttribute(
                        'data-chatterino-rumble-consent-disabled'),
                    pointerEvents: readStyle(element, 'pointer-events'),
                    userSelect: readStyle(element, 'user-select'),
                    filter: readStyle(element, 'filter'),
                    opacity: readStyle(element, 'opacity'),
                });
            }
            element.setAttribute('inert', '');
            element.setAttribute('aria-disabled', 'true');
            element.setAttribute(
                'data-chatterino-rumble-consent-disabled', '');
            element.style.setProperty('pointer-events', 'none', 'important');
            element.style.setProperty('user-select', 'none', 'important');
            element.style.setProperty('filter', 'grayscale(1)', 'important');
            element.style.setProperty('opacity', '0.55', 'important');
        }
        if (!state.observer) {
            state.observer = new MutationObserver(state.disableRumble);
            state.observer.observe(document.body, {childList: true});
        }
    };

    state.enableRumble = () => {
        if (state.observer) {
            state.observer.disconnect();
            state.observer = null;
        }
        for (const [element, original] of state.locked) {
            if (!element.isConnected) {
                continue;
            }
            restoreAttribute(element, 'inert', original.inert);
            restoreAttribute(
                element, 'aria-disabled', original.ariaDisabled);
            restoreAttribute(
                element,
                'data-chatterino-rumble-consent-disabled',
                original.marker);
            restoreStyle(
                element, 'pointer-events', original.pointerEvents);
            restoreStyle(element, 'user-select', original.userSelect);
            restoreStyle(element, 'filter', original.filter);
            restoreStyle(element, 'opacity', original.opacity);
        }
        state.locked.clear();
    };

    state.resolve = () => {
        state.value = 'resolved';
        state.enableRumble();
    };
    state.reject = () => {
        state.value = 'rejected';
    };
    state.purposes = consent => {
        if (consent === null || typeof consent !== 'object' ||
            Array.isArray(consent)) {
            return null;
        }
        const purposes =
            consent.purposes !== null &&
            typeof consent.purposes === 'object' &&
            !Array.isArray(consent.purposes)
                ? consent.purposes
                : consent;
        const values = Object.values(purposes);
        if (values.length === 0 ||
            values.some(value => typeof value !== 'boolean')) {
            return null;
        }
        return purposes;
    };
    state.finishFromConsent = consent => {
        const purposes = state.purposes(consent);
        if (!purposes) {
            return false;
        }
        if (purposes.essential_services === true) {
            state.resolve();
        } else {
            state.reject();
        }
        return true;
    };
    state.readConsent = () => {
        if (state.readingConsent ||
            typeof window.ketch !== 'function') {
            return;
        }
        state.readingConsent = true;
        try {
            window.ketch('getConsent', consent => {
                state.readingConsent = false;
                state.finishFromConsent(consent);
            });
        } catch (_) {
            state.readingConsent = false;
        }
    };
    state.hookKetch = () => {
        if (state.hooked || typeof window.ketch !== 'function') {
            return;
        }
        state.hooked = true;
        try {
            window.ketch('on', 'hideExperience', state.readConsent);
        } catch (_) {
            state.hooked = false;
        }
    };

    Object.defineProperty(window, key, {
        value: state,
        configurable: false,
        enumerable: false,
        writable: false,
    });
}

const state = window[key];
state.hookKetch();
state.disableRumble();
return state.value;
)JS");
}

std::optional<SeleniumManagerResult> parseSeleniumManagerOutput(
    const QByteArray &output)
{
    if (output.isEmpty() || output.size() > MAX_MANAGER_OUTPUT_BYTES ||
        output.contains('\0'))
    {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const auto result =
        document.object().value(QStringLiteral("result")).toObject();
    if (result.value(QStringLiteral("code")).toInt(-1) != 0)
    {
        return std::nullopt;
    }
    SeleniumManagerResult parsed{
        .driverPath = result.value(QStringLiteral("driver_path")).toString(),
        .browserPath = result.value(QStringLiteral("browser_path")).toString(),
    };
    if (parsed.driverPath.isEmpty())
    {
        return std::nullopt;
    }
    return parsed;
}

BrowserWindowGeometry fitPortraitBrowserWindow(const QRect &availableGeometry,
                                               double devicePixelRatio)
{
    constexpr int preferredWidth = 750;
    constexpr int preferredHeight = 1000;
    constexpr int margin = 32;

    // The isolated browser is forced to device scale factor 1 so its CLI and
    // WebDriver outer-window bounds use display pixels. Convert Qt's logical
    // screen coordinates once, including the virtual-screen origin.
    const auto scale = std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0
                           ? devicePixelRatio
                           : 1.0;
    const auto screenX = qRound(availableGeometry.x() * scale);
    const auto screenY = qRound(availableGeometry.y() * scale);
    const auto screenWidth =
        std::max(1, qRound(availableGeometry.width() * scale));
    const auto screenHeight =
        std::max(1, qRound(availableGeometry.height() * scale));
    const auto usableWidth = std::max(1, screenWidth - 2 * margin);
    const auto usableHeight = std::max(1, screenHeight - 2 * margin);
    auto width = std::min(preferredWidth, usableWidth);
    auto height = width * preferredHeight / preferredWidth;
    if (height > usableHeight)
    {
        height = usableHeight;
        width = std::max(1, height * preferredWidth / preferredHeight);
    }
    return {
        .x = screenX + std::max(0, (screenWidth - width) / 2),
        .y = screenY + std::max(0, (screenHeight - height) / 2),
        .width = width,
        .height = height,
    };
}

QStringList chromiumBrowserArguments(const QString &profile,
                                     std::uint16_t debuggingPort,
                                     const BrowserWindowGeometry &geometry,
                                     bool forceX11)
{
    // Binding app mode to the real login URL keeps the frameless app surface
    // across Rumble's auth redirect. Starting app mode at about:blank lets
    // current Chromium reopen the redirect in ordinary tabbed browser chrome.
    QStringList arguments{
        QStringLiteral("--app=") + QString::fromLatin1(LOGIN_URL),
        QStringLiteral("--window-size=%1,%2")
            .arg(geometry.width)
            .arg(geometry.height),
        QStringLiteral("--window-position=%1,%2")
            .arg(geometry.x)
            .arg(geometry.y),
        QStringLiteral("--force-device-scale-factor=1"),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--disable-background-mode"),
        QStringLiteral("--remote-debugging-address=127.0.0.1"),
        QStringLiteral("--remote-debugging-port=%1").arg(debuggingPort),
        QStringLiteral("--user-data-dir=") + profile,
    };
    if (forceX11)
    {
        // Xephyr's root-window capture cannot see Chromium's GPU overlay.
        // This test-only switch keeps screenshots and xdotool automation
        // deterministic without affecting the normal Wayland/Windows launch.
        arguments.prepend(QStringLiteral("--disable-gpu"));
        arguments.prepend(QStringLiteral("--ozone-platform=x11"));
    }
    return arguments;
}

QStringList firefoxBrowserArguments(const BrowserWindowGeometry &geometry)
{
    return {
        QStringLiteral("--width=%1").arg(geometry.width),
        QStringLiteral("--height=%1").arg(geometry.height),
    };
}

QJsonObject chromiumAttachOptions(std::uint16_t debuggingPort)
{
    return {
        {QStringLiteral("debuggerAddress"),
         QStringLiteral("127.0.0.1:%1").arg(debuggingPort)},
    };
}

struct BrowserLogin::State : std::enable_shared_from_this<State> {
    using HttpCallback = std::function<void(int, QByteArray)>;

    QPointer<QObject> owner;
    QPointer<QProcess> managerProcess;
    QPointer<QProcess> loginProcess;
    QPointer<QProcess> browserProcess;
    QPointer<QNetworkAccessManager> network;
    std::unique_ptr<QTemporaryDir> temporary;
    QByteArray output;
    QByteArray managerOutput;
    Callback callback;
    std::vector<PrepareCallback> prepareCallbacks;
    BrowserLoginStatus preparation;
    std::optional<BrowserDescriptor> browser;
    QString managerPath;
    QString driverPath;
    QString externalHelper;
    QString sessionID;
    BrowserWindowGeometry browserGeometry;
    BrowserLoginResult pendingResult;
    quint16 driverPort = 0;
    quint16 browserDebugPort = 0;
    int cookieErrors = 0;
    int browserReadyAttempts = 0;
    int loginDocumentAttempts = 0;
    QString cookieError;
    BrowserConsentState consentState = BrowserConsentState::Pending;
    bool running = false;
    bool cancelling = false;
    bool cleaning = false;
    bool driverReadyRequest = false;
    bool browserReadyRequest = false;

    explicit State(QObject *owner)
        : owner(owner)
    {
    }

    void publishPreparation(BrowserLoginStatus status)
    {
        this->preparation = std::move(status);
        auto callbacks = std::exchange(this->prepareCallbacks, {});
        for (auto &callback : callbacks)
        {
            if (callback)
            {
                callback(this->preparation);
            }
        }
    }

    void prepare(PrepareCallback callback)
    {
        if (callback)
        {
            if (this->preparation.readiness == BrowserLoginReadiness::Ready)
            {
                callback(this->preparation);
                return;
            }
            this->prepareCallbacks.push_back(std::move(callback));
        }
        if (this->preparation.readiness == BrowserLoginReadiness::Preparing)
        {
            return;
        }
        this->preparation = {};

        this->externalHelper =
            qEnvironmentVariable("CHATTERINO_RUMBLE_LOGIN_HELPER");
        if (isExecutableFile(this->externalHelper))
        {
            this->publishPreparation({
                .readiness = BrowserLoginReadiness::Ready,
                .browserName = QStringLiteral("test browser"),
                .userMessage = QStringLiteral("Rumble sign-in is ready."),
            });
            return;
        }
        this->externalHelper.clear();

        this->browser = discoverBrowser();
        if (!this->browser)
        {
            this->publishPreparation({
                .readiness = BrowserLoginReadiness::Unavailable,
                .userMessage = QStringLiteral(
                    "Install Firefox, Chrome, Chromium, Edge, or Brave to "
                    "sign in to Rumble."),
            });
            return;
        }
        this->managerPath = discoverSeleniumManager();
        if (!isExecutableFile(this->managerPath))
        {
            this->publishPreparation({
                .readiness = BrowserLoginReadiness::Unavailable,
                .browserName = this->browser->displayName,
                .userMessage =
                    QStringLiteral(
                        "%1 cannot be used for Rumble sign-in. Try another "
                        "browser or reinstall Chatterino.")
                        .arg(this->browser->displayName),
            });
            return;
        }

        this->preparation = {
            .readiness = BrowserLoginReadiness::Preparing,
            .browserName = this->browser->displayName,
            .userMessage =
                QStringLiteral("Preparing %1 for Rumble sign-in…")
                    .arg(this->browser->displayName),
        };
        auto *process = new QProcess(this->owner);
        this->managerProcess = process;
        this->managerOutput.clear();
        const auto cache =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/browser-drivers");
        QDir().mkpath(cache);
        process->setProgram(this->managerPath);
        process->setArguments({
            QStringLiteral("--browser"),
            this->browser->managerName,
            QStringLiteral("--browser-path"),
            this->browser->executable,
            QStringLiteral("--cache-path"),
            cache,
            QStringLiteral("--avoid-browser-download"),
            QStringLiteral("--avoid-stats"),
            QStringLiteral("--output"),
            QStringLiteral("JSON"),
        });
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setStandardErrorFile(QProcess::nullDevice());

        const std::weak_ptr weak = this->shared_from_this();
        QObject::connect(
            process, &QProcess::readyReadStandardOutput, process, [weak] {
                if (const auto self = weak.lock(); self && self->managerProcess)
                {
                    self->managerOutput.append(
                        self->managerProcess->readAllStandardOutput());
                    if (self->managerOutput.size() > MAX_MANAGER_OUTPUT_BYTES)
                    {
                        self->managerProcess->kill();
                    }
                }
            });
        QObject::connect(
            process, &QProcess::errorOccurred, process,
            [weak](QProcess::ProcessError error) {
                if (const auto self = weak.lock();
                    self && error == QProcess::FailedToStart)
                {
                    self->publishPreparation({
                        .readiness = BrowserLoginReadiness::Unavailable,
                        .browserName = self->browser
                                           ? self->browser->displayName
                                           : QString{},
                        .userMessage =
                            QStringLiteral(
                                "Rumble sign-in preparation could not start. "
                                "Try again."),
                    });
                }
            });
        QObject::connect(
            process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            process, [weak, process](int, QProcess::ExitStatus) {
                const auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                if (self->managerProcess)
                {
                    self->managerOutput.append(
                        self->managerProcess->readAllStandardOutput());
                }
                auto parsed = parseSeleniumManagerOutput(self->managerOutput);
                self->managerOutput.clear();
                self->managerProcess = nullptr;
                process->deleteLater();
                if (!parsed || !isExecutableFile(parsed->driverPath))
                {
                    self->publishPreparation({
                        .readiness = BrowserLoginReadiness::Failed,
                        .browserName = self->browser
                                           ? self->browser->displayName
                                           : QString{},
                        .userMessage = QStringLiteral(
                            "Could not prepare Rumble sign-in. Check your "
                            "connection and try again."),
                    });
                    return;
                }
                self->driverPath = parsed->driverPath;
                if (self->browser && !parsed->browserPath.isEmpty() &&
                    isExecutableFile(parsed->browserPath))
                {
                    self->browser->executable = parsed->browserPath;
                }
                self->publishPreparation({
                    .readiness = BrowserLoginReadiness::Ready,
                    .browserName = self->browser->displayName,
                    .userMessage = QStringLiteral("%1 is ready for Rumble "
                                                   "sign-in.")
                                       .arg(self->browser->displayName),
                });
            });
        process->start(QIODevice::ReadOnly);
        const auto guardedProcess = QPointer<QProcess>(process);
        QTimer::singleShot(
            MANAGER_TIMEOUT_MS, this->owner, [weak, guardedProcess] {
                const auto self = weak.lock();
                if (self && guardedProcess &&
                    self->managerProcess == guardedProcess &&
                    guardedProcess->state() != QProcess::NotRunning)
                {
                    guardedProcess->kill();
                }
            });
    }

    void deliver(BrowserLoginResult loginResult)
    {
        if (!this->running)
        {
            wipe(loginResult.session);
            return;
        }
        this->running = false;
        this->cleaning = false;
        if (this->cancelling)
        {
            wipe(loginResult.session);
            loginResult = cancelled();
        }
        wipe(this->output);
        this->sessionID.clear();
        this->temporary.reset();
        if (this->network)
        {
            this->network->deleteLater();
            this->network = nullptr;
        }
        auto callback = std::move(this->callback);
        this->loginProcess = nullptr;
        this->browserProcess = nullptr;
        if (callback)
        {
            callback(std::move(loginResult));
        }
        else
        {
            wipe(loginResult.session);
        }
    }

    static bool processRunning(const QPointer<QProcess> &process)
    {
        return process && process->state() != QProcess::NotRunning;
    }

    void finishNativeCleanup()
    {
        if (!this->cleaning || processRunning(this->loginProcess) ||
            processRunning(this->browserProcess))
        {
            return;
        }
        this->deliver(std::move(this->pendingResult));
    }

    void stopNativeProcesses()
    {
        for (const auto &process : {this->loginProcess, this->browserProcess})
        {
            if (processRunning(process))
            {
                process->terminate();
            }
        }
        const std::weak_ptr weak = this->shared_from_this();
        if (this->owner)
        {
            QTimer::singleShot(3000, this->owner, [weak] {
                const auto self = weak.lock();
                if (!self || !self->cleaning)
                {
                    return;
                }
                for (const auto &process :
                     {self->loginProcess, self->browserProcess})
                {
                    if (processRunning(process))
                    {
                        process->kill();
                    }
                }
            });
        }
        this->finishNativeCleanup();
    }

    void stopDriver()
    {
        if (!this->externalHelper.isEmpty())
        {
            if (!processRunning(this->loginProcess))
            {
                this->deliver(std::move(this->pendingResult));
                return;
            }
            this->loginProcess->terminate();
            const std::weak_ptr weak = this->shared_from_this();
            if (this->owner)
            {
                QTimer::singleShot(3000, this->owner, [weak] {
                    if (const auto self = weak.lock();
                        self && self->cleaning &&
                        processRunning(self->loginProcess))
                    {
                        self->loginProcess->kill();
                    }
                });
            }
            return;
        }
        this->stopNativeProcesses();
    }

    void httpOnPort(quint16 port, QString method, QString path, QByteArray body,
                    HttpCallback callback)
    {
        if (!this->network)
        {
            callback(0, {});
            return;
        }
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setTransferTimeout(10000);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        QNetworkReply *reply = nullptr;
        if (method == QStringLiteral("GET"))
        {
            reply = this->network->get(request);
        }
        else if (method == QStringLiteral("POST"))
        {
            reply = this->network->post(request, body);
        }
        else
        {
            reply = this->network->sendCustomRequest(request, method.toLatin1(),
                                                     body);
        }
        const std::weak_ptr weak = this->shared_from_this();
        QObject::connect(
            reply, &QNetworkReply::finished, reply,
            [weak, reply, callback = std::move(callback)]() mutable {
                const auto self = weak.lock();
                if (!self)
                {
                    reply->deleteLater();
                    return;
                }
                const auto status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                auto response = reply->read(MAX_DRIVER_RESPONSE_BYTES + 1);
                if (response.size() > MAX_DRIVER_RESPONSE_BYTES)
                {
                    response.clear();
                }
                reply->deleteLater();
                callback(status, std::move(response));
            });
    }

    void http(QString method, QString path, QByteArray body,
              HttpCallback callback)
    {
        this->httpOnPort(this->driverPort, std::move(method), std::move(path),
                         std::move(body), std::move(callback));
    }

    void beginCleanup(BrowserLoginResult loginResult)
    {
        if (!this->running || this->cleaning)
        {
            wipe(loginResult.session);
            return;
        }
        this->cleaning = true;
        this->pendingResult = std::move(loginResult);
        if (!this->externalHelper.isEmpty())
        {
            this->stopDriver();
            return;
        }
        if (this->sessionID.isEmpty())
        {
            this->stopDriver();
            return;
        }
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID));
        const std::weak_ptr weak = this->shared_from_this();
        this->http(QStringLiteral("DELETE"), path, {}, [weak](int, QByteArray) {
            if (const auto self = weak.lock())
            {
                self->sessionID.clear();
                self->stopDriver();
            }
        });
    }

    void pollCookies()
    {
        if (!this->running || this->cleaning || this->sessionID.isEmpty())
        {
            return;
        }
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/cookie");
        const std::weak_ptr weak = this->shared_from_this();
        this->http(
            QStringLiteral("GET"), path, {},
            [weak](int status, QByteArray body) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(body, &error);
                if (status >= 200 && status < 300 &&
                    error.error == QJsonParseError::NoError)
                {
                    self->cookieErrors = 0;
                    self->cookieError.clear();
                    if (auto session =
                            sessionCookie(document.object()
                                              .value(QStringLiteral("value"))
                                              .toArray()))
                    {
                        self->pollAccountName(std::move(*session));
                        return;
                    }
                }
                else
                {
                    self->cookieError = cookieReadFailure(status, body);
                    if (++self->cookieErrors >= 5)
                    {
                        self->beginCleanup(
                            result(BrowserLoginOutcome::Failed,
                                   std::move(self->cookieError)));
                        return;
                    }
                }
                QTimer::singleShot(100, self->owner, [weak] {
                    if (const auto locked = weak.lock())
                    {
                        locked->pollConsentState();
                    }
                });
            });
    }

    void pollConsentState()
    {
        if (!this->running || this->cleaning || this->sessionID.isEmpty())
        {
            return;
        }
        if (this->consentState == BrowserConsentState::Resolved)
        {
            this->pollCookies();
            return;
        }
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/execute/sync");
        const auto body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("script"), browserConsentMonitorScript()},
                    {QStringLiteral("args"), QJsonArray{}},
                })
                .toJson(QJsonDocument::Compact);
        const std::weak_ptr weak = this->shared_from_this();
        this->http(
            QStringLiteral("POST"), path, body,
            [weak](int status, QByteArray response) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                if (status >= 200 && status < 300)
                {
                    if (const auto consent = parseBrowserConsentState(response))
                    {
                        if (*consent == BrowserConsentState::Resolved)
                        {
                            self->consentState = *consent;
                        }
                        else if (*consent == BrowserConsentState::Rejected)
                        {
                            self->beginCleanup(result(
                                BrowserLoginOutcome::Cancelled,
                                QStringLiteral(
                                    "Rumble sign-in was cancelled because "
                                    "cookie consent was rejected.")));
                            return;
                        }
                        else if (*consent ==
                                     BrowserConsentState::ExternalPage &&
                                 self->consentState ==
                                     BrowserConsentState::Pending)
                        {
                            self->beginCleanup(result(
                                BrowserLoginOutcome::Failed,
                                QStringLiteral(
                                    "Rumble's cookie choice is still "
                                    "waiting. Restart sign-in and choose "
                                    "Accept All or Essential Only before "
                                    "selecting Apple, Google, Facebook, or "
                                    "another external identity provider.")));
                            return;
                        }
                    }
                }
                self->pollCookies();
            });
    }

    void pollAccountName(QByteArray session, int attempt = 0)
    {
        if (!this->running || this->cleaning || this->sessionID.isEmpty())
        {
            wipe(session);
            return;
        }
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/execute/sync");
        const auto body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("script"),
                     QStringLiteral(
                         "let name = null;"
                         "if (location.protocol === 'https:' && "
                         "(location.hostname === 'rumble.com' || "
                         "location.hostname === 'www.rumble.com')) {"
                         "const user = window.$$ && window.$$.user;"
                         "name = user && user.logged_in === true && "
                         "typeof user.username === 'string' ? "
                         "user.username : null;"
                         "}"
                         "if (document.documentElement) {"
                         "document.documentElement.style.visibility = "
                         "'hidden';"
                         "document.title = 'Rumble sign-in complete';"
                         "}"
                         "return name;")},
                    {QStringLiteral("args"), QJsonArray{}},
                })
                .toJson(QJsonDocument::Compact);
        const std::weak_ptr weak = this->shared_from_this();
        this->http(
            QStringLiteral("POST"), path, body,
            [weak, session = std::move(session), attempt](
                int status, QByteArray response) mutable {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    wipe(session);
                    return;
                }
                if (status >= 200 && status < 300)
                {
                    if (auto accountName = parseBrowserAccountName(response))
                    {
                        self->beginCleanup({
                            .outcome = BrowserLoginOutcome::Session,
                            .session = std::move(session),
                            .accountName = std::move(*accountName),
                            .userMessage = QStringLiteral(
                                "Rumble sign-in completed; confirming the "
                                "account…"),
                        });
                        return;
                    }
                }
                // The first script invocation hides the completed document.
                // Briefly wait for Rumble's bootstrap to expose a display name;
                // the separate session probe remains authoritative.
                if (attempt >= 20)
                {
                    self->beginCleanup({
                        .outcome = BrowserLoginOutcome::Session,
                        .session = std::move(session),
                        .userMessage =
                            QStringLiteral("Rumble sign-in completed; "
                                           "confirming the account…"),
                    });
                    return;
                }
                QTimer::singleShot(
                    100, self->owner,
                    [weak, session = std::move(session), attempt]() mutable {
                        if (const auto locked = weak.lock())
                        {
                            locked->pollAccountName(std::move(session),
                                                    attempt + 1);
                        }
                        else
                        {
                            wipe(session);
                        }
                    });
            });
    }

    void navigate()
    {
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/url");
        const auto body = QJsonDocument(QJsonObject{
                                            {QStringLiteral("url"),
                                             QString::fromLatin1(LOGIN_URL)},
                                        })
                              .toJson(QJsonDocument::Compact);
        const std::weak_ptr weak = this->shared_from_this();
        this->http(
            QStringLiteral("POST"), path, body, [weak](int status, QByteArray) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                if (status < 200 || status >= 300)
                {
                    self->beginCleanup(
                        result(BrowserLoginOutcome::Failed,
                               QStringLiteral("The Rumble login page could not "
                                              "open.")));
                    return;
                }
                self->waitForLoginDocument();
            });
    }

    void waitForLoginDocument()
    {
        if (!this->running || this->cleaning || this->sessionID.isEmpty())
        {
            return;
        }
        const std::weak_ptr weak = this->shared_from_this();
        this->cdp(
            QStringLiteral("Runtime.evaluate"),
            {
                {QStringLiteral("expression"),
                 QStringLiteral("location.protocol === 'https:' && "
                                "(location.hostname === 'auth.rumble.com' || "
                                "location.hostname === 'rumble.com' || "
                                "location.hostname === 'www.rumble.com') && "
                                "document.readyState !== 'loading'")},
                {QStringLiteral("returnByValue"), true},
            },
            [weak](int status, QByteArray response) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(response, &error);
                if (status >= 200 && status < 300 &&
                    error.error == QJsonParseError::NoError &&
                    document.object()
                        .value(QStringLiteral("value"))
                        .toObject()
                        .value(QStringLiteral("result"))
                        .toObject()
                        .value(QStringLiteral("value"))
                        .toBool())
                {
                    self->pollConsentState();
                    return;
                }
                if (++self->loginDocumentAttempts >= 300)
                {
                    self->beginCleanup(result(
                        BrowserLoginOutcome::Failed,
                        QStringLiteral(
                            "The Rumble login page did not become ready.")));
                    return;
                }
                QTimer::singleShot(100, self->owner, [weak] {
                    if (const auto locked = weak.lock())
                    {
                        locked->waitForLoginDocument();
                    }
                });
            });
    }

    void configureWindow()
    {
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/window/rect");
        const auto body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("x"), this->browserGeometry.x},
                    {QStringLiteral("y"), this->browserGeometry.y},
                    {QStringLiteral("width"), this->browserGeometry.width},
                    {QStringLiteral("height"), this->browserGeometry.height},
                })
                .toJson(QJsonDocument::Compact);
        const std::weak_ptr weak = this->shared_from_this();
        this->http(QStringLiteral("POST"), path, body, [weak](int, QByteArray) {
            if (const auto self = weak.lock();
                self && self->running && !self->cleaning)
            {
                // Firefox needs an explicit outer position after Geckodriver
                // launches it. Navigation remains safe even if the window
                // manager rejects the request.
                self->navigate();
            }
        });
    }

    void cdp(QString command, QJsonObject parameters, HttpCallback callback)
    {
        const auto path =
            QStringLiteral("/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(this->sessionID)) +
            QStringLiteral("/goog/cdp/execute");
        const auto body =
            QJsonDocument(QJsonObject{
                              {QStringLiteral("cmd"), std::move(command)},
                              {QStringLiteral("params"), std::move(parameters)},
                          })
                .toJson(QJsonDocument::Compact);
        this->http(QStringLiteral("POST"), path, body, std::move(callback));
    }

    void createSession()
    {
        if (!this->browser || !this->temporary)
        {
            this->beginCleanup(failed());
            return;
        }
        QJsonObject capabilities{
            // Login providers can keep a document loading while the visible
            // form is already usable. Navigation must not block cookie polling
            // on third-party resources completing.
            {QStringLiteral("pageLoadStrategy"), QStringLiteral("none")},
            {QStringLiteral("unhandledPromptBehavior"),
             QStringLiteral("ignore")},
        };
        if (this->browser->kind == BrowserKind::Firefox)
        {
            capabilities.insert(QStringLiteral("browserName"),
                                QStringLiteral("firefox"));
            capabilities.insert(
                QStringLiteral("moz:firefoxOptions"),
                QJsonObject{
                    {QStringLiteral("binary"), this->browser->executable},
                    {QStringLiteral("args"),
                     QJsonArray::fromStringList(
                         firefoxBrowserArguments(this->browserGeometry))},
                });
        }
        else
        {
            const auto edge = this->browser->kind == BrowserKind::Edge;
            capabilities.insert(QStringLiteral("browserName"),
                                edge ? QStringLiteral("MicrosoftEdge")
                                     : QStringLiteral("chrome"));
            capabilities.insert(edge ? QStringLiteral("ms:edgeOptions")
                                     : QStringLiteral("goog:chromeOptions"),
                                chromiumAttachOptions(this->browserDebugPort));
        }
        const auto body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("capabilities"),
                     QJsonObject{
                         {QStringLiteral("alwaysMatch"), capabilities},
                     }},
                })
                .toJson(QJsonDocument::Compact);
        const std::weak_ptr weak = this->shared_from_this();
        this->http(
            QStringLiteral("POST"), QStringLiteral("/session"), body,
            [weak](int status, QByteArray response) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(response, &error);
                const auto root = document.object();
                const auto value =
                    root.value(QStringLiteral("value")).toObject();
                auto id = value.value(QStringLiteral("sessionId")).toString();
                if (id.isEmpty())
                {
                    id = root.value(QStringLiteral("sessionId")).toString();
                }
                if (status < 200 || status >= 300 ||
                    error.error != QJsonParseError::NoError || id.isEmpty() ||
                    id.size() > 256)
                {
                    self->beginCleanup(unavailable(
                        QStringLiteral("%1 could not start an isolated "
                                       "Rumble login window.")
                            .arg(self->browser->displayName)));
                    return;
                }
                self->sessionID = std::move(id);
                if (self->browser->kind == BrowserKind::Firefox)
                {
                    self->configureWindow();
                }
                else
                {
                    // Chromium was launched with the fitted outer bounds.
                    // Reapplying them through ChromeDriver makes an attached
                    // app-mode window non-resizable on current Linux builds.
                    self->navigate();
                }
            });
    }

    void waitForDriver()
    {
        if (!this->running || this->cleaning || this->driverReadyRequest)
        {
            return;
        }
        this->driverReadyRequest = true;
        const std::weak_ptr weak = this->shared_from_this();
        this->http(QStringLiteral("GET"), QStringLiteral("/status"), {},
                   [weak](int status, QByteArray body) {
                       const auto self = weak.lock();
                       if (!self || !self->running || self->cleaning)
                       {
                           return;
                       }
                       self->driverReadyRequest = false;
                       QJsonParseError error;
                       const auto document =
                           QJsonDocument::fromJson(body, &error);
                       if (status >= 200 && status < 300 &&
                           error.error == QJsonParseError::NoError)
                       {
                           self->createSession();
                           return;
                       }
                       QTimer::singleShot(100, self->owner, [weak] {
                           if (const auto locked = weak.lock())
                           {
                               locked->waitForDriver();
                           }
                       });
                   });
    }

    void appendExternalOutput()
    {
        if (!this->loginProcess || !this->running)
        {
            return;
        }
        this->output.append(this->loginProcess->readAllStandardOutput());
        if (this->output.size() > MAX_OUTPUT_BYTES)
        {
            this->loginProcess->kill();
        }
    }

    bool startExternal(Callback callback)
    {
        auto *process = new QProcess(this->owner);
        process->setProgram(this->externalHelper);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setStandardErrorFile(QProcess::nullDevice());
        this->loginProcess = process;
        this->callback = std::move(callback);
        this->running = true;
        this->cancelling = false;
        this->cleaning = false;
        wipe(this->output);

        const std::weak_ptr weak = this->shared_from_this();
        QObject::connect(process, &QProcess::readyReadStandardOutput, process,
                         [weak] {
                             if (const auto self = weak.lock())
                             {
                                 self->appendExternalOutput();
                             }
                         });
        QObject::connect(
            process, &QProcess::errorOccurred, process,
            [weak](QProcess::ProcessError error) {
                if (const auto self = weak.lock();
                    self && self->running && error == QProcess::FailedToStart)
                {
                    self->deliver(unavailable());
                }
            });
        QObject::connect(
            process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            process, [weak, process](int, QProcess::ExitStatus) {
                if (const auto self = weak.lock(); self && self->running)
                {
                    self->appendExternalOutput();
                    self->deliver(parseBrowserLoginOutput(
                        std::exchange(self->output, {})));
                }
            });
        process->start(QIODevice::ReadOnly);
        return true;
    }

    void startDriver()
    {
        if (!this->running || this->cleaning || !this->browser ||
            this->driverPath.isEmpty() || this->loginProcess)
        {
            return;
        }
        auto *process = new QProcess(this->owner);
        QStringList driverArguments;
        if (this->browser->kind == BrowserKind::Firefox)
        {
            driverArguments = {
                QStringLiteral("--port"),
                QString::number(this->driverPort),
            };
        }
        else
        {
            driverArguments = {
                QStringLiteral("--port=%1").arg(this->driverPort),
            };
        }
        const auto driverLoader =
            qEnvironmentVariable("CHATTERINO_WEBDRIVER_LOADER");
        const auto driverLibraries =
            qEnvironmentVariable("CHATTERINO_WEBDRIVER_LIBRARY_PATH");
        if (!driverLoader.isEmpty() && !driverLibraries.isEmpty() &&
            isExecutableFile(driverLoader))
        {
            process->setProgram(driverLoader);
            QStringList wrappedArguments{
                QStringLiteral("--library-path"),
                driverLibraries,
                this->driverPath,
            };
            wrappedArguments.append(driverArguments);
            driverArguments = std::move(wrappedArguments);
        }
        else
        {
            process->setProgram(this->driverPath);
        }
        process->setArguments(driverArguments);
        process->setProcessChannelMode(QProcess::MergedChannels);
        // WebDriver protocol output can include cookie command responses.
        // Never persist it, including from the attended UI harness.
        process->setStandardOutputFile(QProcess::nullDevice());
        this->loginProcess = process;

        const std::weak_ptr weak = this->shared_from_this();
        QObject::connect(process, &QProcess::started, process, [weak] {
            if (const auto self = weak.lock())
            {
                self->waitForDriver();
            }
        });
        QObject::connect(
            process, &QProcess::errorOccurred, process,
            [weak](QProcess::ProcessError error) {
                if (const auto self = weak.lock();
                    self && self->running && error == QProcess::FailedToStart)
                {
                    self->beginCleanup(unavailable(
                        QStringLiteral("Could not start Rumble sign-in. Try "
                                       "again.")));
                }
            });
        QObject::connect(
            process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            process, [weak, process](int, QProcess::ExitStatus) {
                if (const auto self = weak.lock(); self && self->running)
                {
                    self->loginProcess = nullptr;
                    process->deleteLater();
                    if (self->cleaning)
                    {
                        self->finishNativeCleanup();
                    }
                    else
                    {
                        self->sessionID.clear();
                        self->beginCleanup(result(
                            BrowserLoginOutcome::Failed,
                            QStringLiteral("Rumble sign-in stopped before it "
                                           "finished. Try again.")));
                    }
                }
            });
        process->start(QIODevice::ReadOnly);
    }

    void waitForBrowser()
    {
        if (!this->running || this->cleaning || this->browserReadyRequest ||
            !this->browserProcess)
        {
            return;
        }
        this->browserReadyRequest = true;
        const std::weak_ptr weak = this->shared_from_this();
        this->httpOnPort(
            this->browserDebugPort, QStringLiteral("GET"),
            QStringLiteral("/json/version"), {},
            [weak](int status, QByteArray body) {
                const auto self = weak.lock();
                if (!self || !self->running || self->cleaning)
                {
                    return;
                }
                self->browserReadyRequest = false;
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(body, &error);
                const auto socket =
                    QUrl(document.object()
                             .value(QStringLiteral("webSocketDebuggerUrl"))
                             .toString());
                if (status >= 200 && status < 300 &&
                    error.error == QJsonParseError::NoError &&
                    socket.scheme() == QStringLiteral("ws") &&
                    QHostAddress(socket.host()).isLoopback() &&
                    socket.port() == self->browserDebugPort &&
                    supportedChromiumProduct(
                        document.object()
                            .value(QStringLiteral("Browser"))
                            .toString()))
                {
                    self->startDriver();
                    return;
                }
                if (++self->browserReadyAttempts >= 150)
                {
                    self->beginCleanup(unavailable(
                        QStringLiteral("Could not open the Rumble sign-in "
                                       "window. Try again.")));
                    return;
                }
                QTimer::singleShot(100, self->owner, [weak] {
                    if (const auto locked = weak.lock())
                    {
                        locked->waitForBrowser();
                    }
                });
            });
    }

    void startChromiumBrowser()
    {
        if (!this->browser || !this->temporary)
        {
            this->beginCleanup(failed());
            return;
        }
        auto *process = new QProcess(this->owner);
        process->setProgram(this->browser->executable);
        process->setArguments(chromiumBrowserArguments(
            this->temporary->path() + QStringLiteral("/profile"),
            this->browserDebugPort, this->browserGeometry,
            qEnvironmentVariable("CHATTERINO_RUMBLE_LOGIN_TEST_X11") ==
                QStringLiteral("1")));
        process->setProcessChannelMode(QProcess::MergedChannels);
        // Browser output can contain navigated addresses. Never persist it.
        process->setStandardOutputFile(QProcess::nullDevice());
        this->browserProcess = process;

        const std::weak_ptr weak = this->shared_from_this();
        QObject::connect(process, &QProcess::started, process, [weak] {
            if (const auto self = weak.lock())
            {
                self->waitForBrowser();
            }
        });
        QObject::connect(
            process, &QProcess::errorOccurred, process,
            [weak](QProcess::ProcessError error) {
                if (const auto self = weak.lock();
                    self && self->running && error == QProcess::FailedToStart)
                {
                    self->beginCleanup(unavailable(
                        QStringLiteral("Could not open the Rumble sign-in "
                                       "window. Try again.")));
                }
            });
        QObject::connect(
            process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            process, [weak, process](int, QProcess::ExitStatus) {
                if (const auto self = weak.lock(); self && self->running)
                {
                    self->browserProcess = nullptr;
                    process->deleteLater();
                    if (self->cleaning)
                    {
                        self->finishNativeCleanup();
                    }
                    else
                    {
                        self->sessionID.clear();
                        self->beginCleanup(result(
                            BrowserLoginOutcome::Failed,
                            QStringLiteral(
                                "The sign-in window closed before Rumble "
                                "sign-in finished. Try again.")));
                    }
                }
            });
        process->start(QIODevice::ReadOnly);
    }

    bool startNative(Callback callback)
    {
        if (!this->browser || this->driverPath.isEmpty())
        {
            callback(unavailable());
            return false;
        }
        auto reservePort = []() -> std::optional<quint16> {
            QTcpServer portProbe;
            if (!portProbe.listen(QHostAddress::LocalHost, 0))
            {
                return std::nullopt;
            }
            return portProbe.serverPort();
        };
        const auto driverPort = reservePort();
        if (!driverPort)
        {
            callback(failed());
            return false;
        }
        this->driverPort = *driverPort;
        if (this->browser->kind != BrowserKind::Firefox)
        {
            const auto browserPort = reservePort();
            if (!browserPort)
            {
                callback(failed());
                return false;
            }
            this->browserDebugPort = *browserPort;
        }

        this->temporary = std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/chatterino-rumble-login-XXXXXX"));
        if (!this->temporary->isValid())
        {
            this->temporary.reset();
            callback(failed());
            return false;
        }

        this->network = new QNetworkAccessManager(this->owner);
        this->network->setProxy(QNetworkProxy::NoProxy);
        this->callback = std::move(callback);
        this->running = true;
        this->cancelling = false;
        this->cleaning = false;
        this->driverReadyRequest = false;
        this->browserReadyRequest = false;
        this->browserReadyAttempts = 0;
        this->loginDocumentAttempts = 0;
        this->cookieErrors = 0;
        this->cookieError.clear();
        this->consentState = BrowserConsentState::Pending;
        auto screenBounds = QRect(0, 0, 1280, 800);
        auto screenScale = 1.0;
        auto *screen = QGuiApplication::primaryScreen();
        if (auto *window = QGuiApplication::focusWindow();
            window && window->screen())
        {
            screen = window->screen();
        }
        if (screen)
        {
            screenBounds = screen->availableGeometry();
            screenScale = screen->devicePixelRatio();
        }
        this->browserGeometry =
            fitPortraitBrowserWindow(screenBounds, screenScale);

        if (this->browser->kind == BrowserKind::Firefox)
        {
            this->startDriver();
        }
        else
        {
            this->startChromiumBrowser();
        }

        const std::weak_ptr weak = this->shared_from_this();
        QTimer::singleShot(10 * 60 * 1000, this->owner, [weak] {
            if (const auto self = weak.lock();
                self && self->running && !self->cleaning)
            {
                self->beginCleanup(
                    result(BrowserLoginOutcome::Failed,
                           QStringLiteral("Rumble sign-in took too long. Try "
                                          "again.")));
            }
        });
        return true;
    }

    void stop(bool deliverCancellation) noexcept
    {
        if (!this->running)
        {
            return;
        }
        this->cancelling = true;
        if (!deliverCancellation)
        {
            this->callback = {};
        }
        if (!deliverCancellation)
        {
            for (const auto &process :
                 {this->loginProcess, this->browserProcess})
            {
                if (processRunning(process))
                {
                    process->terminate();
                }
            }
            this->running = false;
            this->cleaning = true;
            this->sessionID.clear();
            wipe(this->output);
            return;
        }
        this->beginCleanup(cancelled());
    }
};

BrowserLogin::BrowserLogin(QObject *owner)
    : state_(std::make_shared<State>(owner))
{
}

BrowserLogin::~BrowserLogin()
{
    this->shutdown();
    const auto state = this->state_;
    for (const auto &process :
         {state ? state->managerProcess : QPointer<QProcess>{},
          state ? state->loginProcess : QPointer<QProcess>{},
          state ? state->browserProcess : QPointer<QProcess>{}})
    {
        if (process && process->state() != QProcess::NotRunning)
        {
            process->waitForFinished(3000);
            if (process && process->state() != QProcess::NotRunning)
            {
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }
}

void BrowserLogin::prepare(PrepareCallback callback)
{
    if (!this->state_ || !this->state_->owner)
    {
        callback({
            .readiness = BrowserLoginReadiness::Unavailable,
            .userMessage = QStringLiteral("Rumble sign-in is unavailable."),
        });
        return;
    }
    this->state_->prepare(std::move(callback));
}

bool BrowserLogin::start(Callback callback)
{
    const auto state = this->state_;
    if (!state || state->running || !state->owner)
    {
        if (callback)
        {
            callback(failed());
        }
        return false;
    }
    if (state->preparation.readiness != BrowserLoginReadiness::Ready)
    {
        if (callback)
        {
            callback(unavailable(state->preparation.userMessage));
        }
        return false;
    }
    if (!state->externalHelper.isEmpty())
    {
        return state->startExternal(std::move(callback));
    }
    return state->startNative(std::move(callback));
}

void BrowserLogin::cancel() noexcept
{
    if (this->state_)
    {
        this->state_->stop(true);
    }
}

void BrowserLogin::shutdown() noexcept
{
    if (!this->state_)
    {
        return;
    }
    this->state_->stop(false);
    if (this->state_->managerProcess &&
        this->state_->managerProcess->state() != QProcess::NotRunning)
    {
        this->state_->managerProcess->terminate();
    }
}

bool BrowserLogin::active() const noexcept
{
    return this->state_ && this->state_->running;
}

BrowserLoginStatus BrowserLogin::status() const
{
    return this->state_
               ? this->state_->preparation
               : BrowserLoginStatus{
                     .readiness = BrowserLoginReadiness::Unavailable,
                     .userMessage = QStringLiteral("Rumble sign-in is "
                                                   "unavailable."),
                 };
}

}  // namespace chatterino::rumble
