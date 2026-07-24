// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino::rumble {

struct Diagnostic {
    QString code;
    QString path;

    friend bool operator==(const Diagnostic &, const Diagnostic &) = default;
};

}  // namespace chatterino::rumble
