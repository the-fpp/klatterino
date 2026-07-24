// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleSseParser.hpp"

#include <utility>

namespace chatterino::rumble {

SseFeedResult SseParser::feed(QByteArrayView bytes)
{
    SseFeedResult result;

    for (const char byte : bytes)
    {
        if (this->discarding_)
        {
            if (byte == '\n')
            {
                if (!this->discardLineHasContent_)
                {
                    this->discarding_ = false;
                    this->pendingBytes_ = 0;
                }
                this->discardLineHasContent_ = false;
            }
            else if (byte != '\r')
            {
                this->discardLineHasContent_ = true;
            }
            continue;
        }

        this->line_.append(byte);
        if (byte != '\n' && byte != '\r')
        {
            ++this->pendingBytes_;
        }

        // The second condition keeps a stream containing pathological CR runs
        // bounded while still allowing a normal CRLF terminator.
        if (this->pendingBytes_ > MAX_EVENT_BYTES ||
            this->line_.size() > MAX_EVENT_BYTES + 2)
        {
            result.diagnostics.push_back(
                {QStringLiteral("event_too_large"),
                 QStringLiteral("sse.event")});
            this->line_.clear();
            this->dataLines_.clear();
            this->discarding_ = true;
            this->discardLineHasContent_ = byte != '\n';
            if (byte == '\r')
            {
                this->discardLineHasContent_ = false;
            }
            continue;
        }

        if (byte == '\n')
        {
            this->line_.chop(1);
            this->processLine(result);
        }
    }

    return result;
}

void SseParser::reset()
{
    this->line_.clear();
    this->dataLines_.clear();
    this->pendingBytes_ = 0;
    this->discarding_ = false;
    this->discardLineHasContent_ = false;
}

void SseParser::processLine(SseFeedResult &result)
{
    auto line = std::exchange(this->line_, {});
    if (line.endsWith('\r'))
    {
        line.chop(1);
    }

    if (line.isEmpty())
    {
        if (!this->dataLines_.empty())
        {
            QByteArray record;
            for (std::size_t index = 0; index < this->dataLines_.size();
                 ++index)
            {
                if (index != 0)
                {
                    record.append('\n');
                }
                record.append(this->dataLines_[index]);
            }
            if (!record.isEmpty())
            {
                result.records.push_back(std::move(record));
            }
        }
        this->dataLines_.clear();
        this->pendingBytes_ = 0;
        return;
    }

    if (line.startsWith(':'))
    {
        return;
    }

    QByteArray value;
    if (line == "data")
    {
        value = {};
    }
    else if (line.startsWith("data:"))
    {
        value = line.sliced(5);
        if (value.startsWith(' '))
        {
            value.remove(0, 1);
        }
    }
    else
    {
        return;
    }

    this->dataLines_.push_back(std::move(value));
}

}  // namespace chatterino::rumble
