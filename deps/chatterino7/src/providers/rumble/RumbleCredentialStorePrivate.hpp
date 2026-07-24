// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleCredentialStore.hpp"

#include <QStringView>
#include <qt6keychain/keychain.h>

namespace chatterino::rumble::detail {

CredentialStoreError mapCredentialStoreError(
    QKeychain::Error error, QStringView errorText = {});

}  // namespace chatterino::rumble::detail
