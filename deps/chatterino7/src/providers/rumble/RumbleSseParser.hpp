// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/rumble/RumbleDiagnostic.hpp"

#include <QByteArray>
#include <QByteArrayView>

#include <vector>

namespace chatterino::rumble {

struct SseFeedResult {
    std::vector<QByteArray> records;
    std::vector<Diagnostic> diagnostics;
};

class SseParser
{
public:
    static constexpr qsizetype MAX_EVENT_BYTES = 1024 * 1024;

    SseFeedResult feed(QByteArrayView bytes);
    void reset();

private:
    void processLine(SseFeedResult &result);

    QByteArray line_;
    std::vector<QByteArray> dataLines_;
    qsizetype pendingBytes_ = 0;
    bool discarding_ = false;
    bool discardLineHasContent_ = false;
};

}  // namespace chatterino::rumble
