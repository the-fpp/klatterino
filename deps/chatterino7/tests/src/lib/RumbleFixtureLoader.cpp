// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureLoader.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <limits>
#include <stdexcept>
#include <utility>

namespace chatterino::test {
namespace {

constexpr auto MANIFEST_PATH = ":/rumble/scenarios.json";

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

QJsonObject requireObject(const QJsonValue &value, const QString &context)
{
    if (!value.isObject())
    {
        fail(context + " must be an object");
    }
    return value.toObject();
}

QJsonArray requireArray(const QJsonValue &value, const QString &context)
{
    if (!value.isArray())
    {
        fail(context + " must be an array");
    }
    return value.toArray();
}

QString requireString(const QJsonObject &object, const QString &key,
                      const QString &context)
{
    const auto value = object.value(key);
    if (!value.isString() || value.toString().isEmpty())
    {
        fail(context + "." + key + " must be a non-empty string");
    }
    return value.toString();
}

int requireInt(const QJsonObject &object, const QString &key,
               const QString &context, int defaultValue,
               bool required = false)
{
    const auto value = object.value(key);
    if (value.isUndefined() && !required)
    {
        return defaultValue;
    }
    if (!value.isDouble())
    {
        fail(context + "." + key + " must be an integer");
    }

    const auto asDouble = value.toDouble();
    const auto asInt = value.toInt(std::numeric_limits<int>::min());
    if (asDouble != static_cast<double>(asInt))
    {
        fail(context + "." + key + " must be an integer");
    }
    return asInt;
}

std::vector<RumbleFixtureHeader> parseHeaders(const QJsonValue &value,
                                              const QString &context)
{
    std::vector<RumbleFixtureHeader> headers;
    if (value.isUndefined())
    {
        return headers;
    }

    const auto array = requireArray(value, context);
    headers.reserve(static_cast<std::size_t>(array.size()));
    for (int index = 0; index < array.size(); ++index)
    {
        const auto itemContext =
            context + "[" + QString::number(index) + "]";
        const auto object = requireObject(array[index], itemContext);
        headers.push_back({
            .name = requireString(object, "name", itemContext).toStdString(),
            .value = requireString(object, "value", itemContext).toStdString(),
        });
    }
    return headers;
}

std::vector<RumbleFixtureChunk> parseChunks(const QJsonValue &value,
                                            const QString &context)
{
    std::vector<RumbleFixtureChunk> chunks;
    if (value.isUndefined())
    {
        return chunks;
    }

    const auto array = requireArray(value, context);
    chunks.reserve(static_cast<std::size_t>(array.size()));
    for (int index = 0; index < array.size(); ++index)
    {
        const auto itemContext =
            context + "[" + QString::number(index) + "]";
        const auto object = requireObject(array[index], itemContext);
        const auto path = requireString(object, "resource", itemContext);
        const auto bytes = readRumbleFixtureResource(path);
        const auto offset = requireInt(object, "offset", itemContext, 0);
        const auto length = requireInt(object, "length", itemContext, -1);
        const auto afterMs =
            requireInt(object, "after_ms", itemContext, 0);

        if (offset < 0 || static_cast<std::size_t>(offset) > bytes.size())
        {
            fail(itemContext + ".offset is outside the fixture resource");
        }
        if (length < -1)
        {
            fail(itemContext + ".length must be -1 or non-negative");
        }
        if (afterMs < 0)
        {
            fail(itemContext + ".after_ms must be non-negative");
        }

        const auto available = bytes.size() - static_cast<std::size_t>(offset);
        const auto count = length == -1 ? available
                                        : static_cast<std::size_t>(length);
        if (count > available)
        {
            fail(itemContext + ".length exceeds the fixture resource");
        }

        chunks.push_back({
            .afterMs = afterMs,
            .bytes = bytes.substr(static_cast<std::size_t>(offset), count),
        });
    }
    return chunks;
}

RumbleFixtureExchange parseExchange(const QJsonValue &value,
                                    const QString &context)
{
    const auto object = requireObject(value, context);
    const auto request =
        requireObject(object.value("request"), context + ".request");
    const auto response =
        requireObject(object.value("response"), context + ".response");

    const auto headAfterMs =
        requireInt(response, "head_after_ms", context + ".response", 0);
    const auto terminalAfterMs = requireInt(
        response, "terminal_after_ms", context + ".response", 0);
    if (headAfterMs < 0 || terminalAfterMs < 0)
    {
        fail(context + " response delays must be non-negative");
    }

    auto terminal = RumbleFixtureTerminal::Complete;
    const auto terminalName =
        requireString(response, "terminal", context + ".response");
    if (terminalName == "disconnect")
    {
        terminal = RumbleFixtureTerminal::Disconnect;
    }
    else if (terminalName != "complete")
    {
        fail(context + ".response.terminal must be complete or disconnect");
    }

    const auto reasonValue = response.value("terminal_reason");
    if (!reasonValue.isUndefined() && !reasonValue.isString())
    {
        fail(context + ".response.terminal_reason must be a string");
    }

    return {
        .label = requireString(object, "label", context).toStdString(),
        .expectedRequest =
            {
                .method = requireString(request, "method",
                                        context + ".request")
                              .toStdString(),
                .target = requireString(request, "target",
                                        context + ".request")
                              .toStdString(),
                .headers = parseHeaders(request.value("headers"),
                                        context + ".request.headers"),
            },
        .response =
            {
                .status = requireInt(response, "status",
                                     context + ".response", 0, true),
                .headers = parseHeaders(response.value("headers"),
                                        context + ".response.headers"),
            },
        .headAfterMs = headAfterMs,
        .chunks = parseChunks(response.value("chunks"),
                              context + ".response.chunks"),
        .terminal = terminal,
        .terminalAfterMs = terminalAfterMs,
        .terminalReason = reasonValue.toString().toStdString(),
    };
}

}  // namespace

std::string readRumbleFixtureResource(const QString &resourcePath)
{
    QFile fixture(resourcePath);
    if (!fixture.open(QIODevice::ReadOnly))
    {
        fail("could not open Rumble fixture resource " + resourcePath);
    }

    const auto bytes = fixture.readAll();
    return std::string(bytes.constData(),
                       static_cast<std::size_t>(bytes.size()));
}

RumbleFixtureScript loadRumbleFixtureScenario(const QString &name)
{
    QFile manifest(MANIFEST_PATH);
    if (!manifest.open(QIODevice::ReadOnly))
    {
        fail("could not open Rumble fixture manifest");
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        fail("invalid Rumble fixture manifest: " + error.errorString());
    }

    const auto root = document.object();
    if (requireInt(root, "schema_version", "manifest", 0, true) != 1)
    {
        fail("unsupported Rumble fixture manifest schema");
    }

    const auto scenarios =
        requireArray(root.value("scenarios"), "manifest.scenarios");
    for (int scenarioIndex = 0; scenarioIndex < scenarios.size();
         ++scenarioIndex)
    {
        const auto scenarioContext =
            "manifest.scenarios[" + QString::number(scenarioIndex) + "]";
        const auto scenario =
            requireObject(scenarios[scenarioIndex], scenarioContext);
        if (requireString(scenario, "name", scenarioContext) != name)
        {
            continue;
        }

        RumbleFixtureScript script;
        script.name = name.toStdString();
        const auto exchanges = requireArray(scenario.value("exchanges"),
                                            scenarioContext + ".exchanges");
        if (exchanges.isEmpty())
        {
            fail(scenarioContext + ".exchanges must not be empty");
        }
        script.exchanges.reserve(
            static_cast<std::size_t>(exchanges.size()));
        for (int exchangeIndex = 0; exchangeIndex < exchanges.size();
             ++exchangeIndex)
        {
            script.exchanges.push_back(parseExchange(
                exchanges[exchangeIndex],
                scenarioContext + ".exchanges[" +
                    QString::number(exchangeIndex) + "]"));
        }
        return script;
    }

    fail("unknown Rumble fixture scenario: " + name);
}

}  // namespace chatterino::test
