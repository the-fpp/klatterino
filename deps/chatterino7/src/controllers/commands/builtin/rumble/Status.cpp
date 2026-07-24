// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/rumble/Status.hpp"

#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleDiagnostics.hpp"

#include <QDateTime>

#include <memory>

namespace chatterino::commands {

QString rumbleStatus(const CommandContext &ctx)
{
    if (!ctx.channel)
        return {};
    const auto channel = std::dynamic_pointer_cast<RumbleChannel>(ctx.channel);
    if (!channel)
    {
        ctx.channel->addSystemMessage(QStringLiteral(
            "The /rumble-status command only works in a Rumble channel."));
        return {};
    }
    const auto snapshot =
        rumble::captureStatus(*channel, QDateTime::currentDateTimeUtc());
    channel->addSystemMessage(
        snapshot
            ? rumble::formatStatus(*snapshot)
            : QStringLiteral("Rumble status is unavailable right now."));
    return {};
}

}  // namespace chatterino::commands
