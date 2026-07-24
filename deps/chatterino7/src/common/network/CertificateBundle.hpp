// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

/// Replaces Qt's default CA set with the PEM certificates in `path`.
/// Returns false without changing the default configuration when the bundle
/// cannot be read or contains no certificates.
[[nodiscard]] bool applyCertificateBundle(const QString &path);

}  // namespace chatterino
