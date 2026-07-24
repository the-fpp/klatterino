// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "providers/rumble/RumbleEvent.hpp"

namespace chatterino::rumble {

MessagePtrMut buildMessage(const MessageDto &message);

}  // namespace chatterino::rumble
