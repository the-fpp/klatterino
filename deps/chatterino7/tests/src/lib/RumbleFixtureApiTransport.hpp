// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "lib/RumbleFixtureTransport.hpp"
#include "providers/rumble/RumbleApi.hpp"

namespace chatterino::test {

// Shared adapter from #18's deterministic scripted transport to the
// production typed Rumble API transport seam.
class RumbleFixtureApiTransport final : public rumble::Transport
{
public:
    explicit RumbleFixtureApiTransport(RumbleFixtureTransport &transport);

    std::unique_ptr<rumble::TransportHandle> start(
        rumble::TransportRequest request,
        rumble::TransportCallbacks callbacks) override;

private:
    RumbleFixtureTransport &transport_;
};

}  // namespace chatterino::test
