// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "lib/RumbleFixtureTransport.hpp"

#include <QString>

#include <string>

namespace chatterino::test {

RumbleFixtureScript loadRumbleFixtureScenario(const QString &name);
std::string readRumbleFixtureResource(const QString &resourcePath);

}  // namespace chatterino::test
