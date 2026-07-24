// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleCredentialStore.hpp"

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include <memory>

namespace {

using chatterino::rumble::CredentialStore;
using chatterino::rumble::CredentialStoreError;

constexpr auto TEST_KEY = "nixos-credential-store-integration";
constexpr auto TEST_SECRET = "SYNTHETIC_CREDENTIAL_STORE_CANARY";

QString errorName(CredentialStoreError error)
{
    switch (error)
    {
        case CredentialStoreError::None:
            return QStringLiteral("none");
        case CredentialStoreError::NotFound:
            return QStringLiteral("not-found");
        case CredentialStoreError::Unavailable:
            return QStringLiteral("unavailable");
        case CredentialStoreError::AccessDenied:
            return QStringLiteral("access-denied");
        case CredentialStoreError::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

void printResult(QStringView operation, bool available, QStringView result,
                 CredentialStoreError error)
{
    QTextStream output(stdout);
    output << "operation=" << operation << '\n'
           << "qtkeychain_available=" << (available ? "true" : "false")
           << '\n'
           << "session_bus_address_set="
           << (qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS") ? "true"
                                                                      : "false")
           << '\n'
           << "result=" << result << '\n'
           << "store_error=" << errorName(error) << '\n';
    if (error != CredentialStoreError::None)
    {
        auto message =
            chatterino::rumble::credentialStoreErrorText(error).simplified();
        output << "diagnostic=" << message << '\n';
    }
    output.flush();
}

void finish(QCoreApplication &app, int exitCode)
{
    QTimer::singleShot(0, &app, [&app, exitCode] {
        app.exit(exitCode);
    });
}

int usage(QStringView program)
{
    QTextStream(stderr)
        << "usage: " << program
        << " write|read|erase|read-missing|expect-unavailable\n";
    return 64;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 2)
    {
        return usage(arguments.value(0));
    }

    const auto operation = arguments.at(1);
    const auto key = QString::fromLatin1(TEST_KEY);
    auto store = chatterino::rumble::makeCredentialStore(&app);
    const auto available = store->available();

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] {
        printResult(operation, available, QStringLiteral("timeout"),
                    CredentialStoreError::Failed);
        app.exit(124);
    });
    timeout.start(15000);

    if (operation == QStringLiteral("write"))
    {
        store->write(
            key, QByteArrayLiteral(TEST_SECRET),
            [&](CredentialStoreError error) {
                printResult(operation, available,
                            error == CredentialStoreError::None
                                ? QStringLiteral("success")
                                : QStringLiteral("error"),
                            error);
                finish(app, error == CredentialStoreError::None ? 0 : 1);
            });
    }
    else if (operation == QStringLiteral("read"))
    {
        store->read(key, [&](chatterino::rumble::CredentialReadResult result) {
            const auto matches =
                result.secret == QByteArrayLiteral(TEST_SECRET);
            result.secret.fill('\0');
            result.secret.clear();
            const auto success =
                result.error == CredentialStoreError::None && matches;
            printResult(operation, available,
                        success ? QStringLiteral("success")
                                : (result.error == CredentialStoreError::None
                                       ? QStringLiteral("secret-mismatch")
                                       : QStringLiteral("error")),
                        result.error);
            finish(app, success ? 0 : 1);
        });
    }
    else if (operation == QStringLiteral("erase"))
    {
        store->erase(key, [&](CredentialStoreError error) {
            printResult(operation, available,
                        error == CredentialStoreError::None
                            ? QStringLiteral("success")
                            : QStringLiteral("error"),
                        error);
            finish(app, error == CredentialStoreError::None ? 0 : 1);
        });
    }
    else if (operation == QStringLiteral("read-missing"))
    {
        store->read(key, [&](chatterino::rumble::CredentialReadResult result) {
            result.secret.fill('\0');
            result.secret.clear();
            const auto success =
                result.error == CredentialStoreError::NotFound;
            printResult(operation, available,
                        success ? QStringLiteral("success")
                                : QStringLiteral("unexpected-result"),
                        result.error);
            finish(app, success ? 0 : 1);
        });
    }
    else if (operation == QStringLiteral("expect-unavailable"))
    {
        store->write(
            key, QByteArrayLiteral(TEST_SECRET),
            [&](CredentialStoreError error) {
                const auto success =
                    error == CredentialStoreError::Unavailable;
                printResult(operation, available,
                            success ? QStringLiteral("success")
                                    : QStringLiteral("unexpected-result"),
                            error);
                finish(app, success ? 0 : 1);
            });
    }
    else
    {
        return usage(arguments.value(0));
    }

    return app.exec();
}
