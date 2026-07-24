// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleCredentialStore.hpp"
#include "providers/rumble/RumbleCredentialStorePrivate.hpp"

#include <gtest/gtest.h>

using chatterino::rumble::CredentialStoreError;
using chatterino::rumble::detail::mapCredentialStoreError;

TEST(RumbleCredentialStore, MapsQtKeychainErrors)
{
    EXPECT_EQ(mapCredentialStoreError(QKeychain::NoError),
              CredentialStoreError::None);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::EntryNotFound),
              CredentialStoreError::NotFound);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::AccessDenied),
              CredentialStoreError::AccessDenied);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::AccessDeniedByUser),
              CredentialStoreError::AccessDenied);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::NoBackendAvailable),
              CredentialStoreError::Unavailable);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::NotImplemented),
              CredentialStoreError::Unavailable);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::CouldNotDeleteEntry),
              CredentialStoreError::Failed);
    EXPECT_EQ(mapCredentialStoreError(QKeychain::OtherError,
                                      QStringLiteral("unrelated failure")),
              CredentialStoreError::Failed);
}

TEST(RumbleCredentialStore, RecognizesMissingLinuxSecretService)
{
    EXPECT_EQ(
        mapCredentialStoreError(
            QKeychain::OtherError,
            QStringLiteral(
                "The name org.freedesktop.secrets was not provided by any "
                ".service files")),
        CredentialStoreError::Unavailable);
    EXPECT_EQ(
        mapCredentialStoreError(
            QKeychain::OtherError,
            QStringLiteral("No Secret Service backend could be activated")),
        CredentialStoreError::Unavailable);
    EXPECT_EQ(
        mapCredentialStoreError(
            QKeychain::OtherError,
            QStringLiteral(
                "Unable to autolaunch a dbus-daemon without a $DISPLAY")),
        CredentialStoreError::Unavailable);
}

TEST(RumbleCredentialStore, UnavailableMessageIsActionable)
{
    const auto message = chatterino::rumble::credentialStoreErrorText(
        CredentialStoreError::Unavailable);

    EXPECT_TRUE(message.contains(QStringLiteral("credential storage"),
                                 Qt::CaseInsensitive));
#if defined(Q_OS_LINUX)
    EXPECT_TRUE(
        message.contains(QStringLiteral("system keyring"), Qt::CaseInsensitive));
#endif
}
