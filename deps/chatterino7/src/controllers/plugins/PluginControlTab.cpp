// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/PluginControlTab.hpp"

#    include "Application.hpp"
#    include "common/QLogging.hpp"
#    include "controllers/plugins/LuaUtilities.hpp"
#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/PluginController.hpp"
#    include "controllers/plugins/PluginPermission.hpp"
#    include "controllers/plugins/SolTypes.hpp"

#    include <QScopeGuard>
#    include <QSet>
#    include <QStringList>
#    include <sol/sol.hpp>

#    include <algorithm>
#    include <cmath>
#    include <limits>
#    include <optional>
#    include <utility>

namespace chatterino::lua::api {
namespace {

constexpr qsizetype MAX_TITLE_LENGTH = 48;
constexpr qsizetype MAX_PRIMARY_LENGTH = 96;
constexpr qsizetype MAX_SECONDARY_LENGTH = 160;
constexpr qsizetype MAX_LABEL_LENGTH = 80;
constexpr qsizetype MAX_DESCRIPTION_LENGTH = 240;
constexpr qsizetype MAX_ACCESSIBLE_LENGTH = 200;
constexpr qsizetype MAX_TOOLTIP_LENGTH = 240;
constexpr qsizetype MAX_ID_LENGTH = 48;
constexpr qsizetype MAX_UNITS_LENGTH = 24;
constexpr std::size_t MAX_CONTROLS = 16;
constexpr std::size_t MAX_CHOICES = 16;
constexpr double MAX_ABSOLUTE_VALUE = 1.0e12;

const QSet<QString> &acceptedIcons()
{
    static const QSet<QString> icons{
        QStringLiteral(""),        QStringLiteral("play"),
        QStringLiteral("pause"),   QStringLiteral("stop"),
        QStringLiteral("refresh"), QStringLiteral("settings"),
        QStringLiteral("info"),    QStringLiteral("warning"),
        QStringLiteral("error"),   QStringLiteral("check"),
        QStringLiteral("status"),
    };
    return icons;
}

QString luaTypeName(sol::type type)
{
    switch (type)
    {
        case sol::type::none:
            return QStringLiteral("missing value");
        case sol::type::nil:
            return QStringLiteral("nil");
        case sol::type::string:
            return QStringLiteral("string");
        case sol::type::number:
            return QStringLiteral("number");
        case sol::type::boolean:
            return QStringLiteral("boolean");
        case sol::type::table:
            return QStringLiteral("table");
        case sol::type::function:
            return QStringLiteral("function");
        case sol::type::userdata:
            return QStringLiteral("userdata");
        case sol::type::lightuserdata:
            return QStringLiteral("light userdata");
        case sol::type::thread:
            return QStringLiteral("thread");
        case sol::type::poly:
            return QStringLiteral("value");
    }
    return QStringLiteral("unknown value");
}

sol::object field(const sol::table &table, const char *key)
{
    return table.raw_get<sol::object>(key);
}

bool hasField(const sol::table &table, const char *key)
{
    const auto value = field(table, key);
    return value.get_type() != sol::type::none &&
           value.get_type() != sol::type::nil;
}

Expected<void, QString> validateKeys(const sol::table &table,
                                     const QSet<QString> &allowed,
                                     QStringView context)
{
    for (const auto &[rawKey, _] : table)
    {
        if (rawKey.get_type() != sol::type::string)
        {
            return makeUnexpected(
                QStringLiteral("%1 contains a non-string field")
                    .arg(context));
        }
        const auto key = rawKey.as<QString>();
        if (!allowed.contains(key))
        {
            return makeUnexpected(
                QStringLiteral("%1 contains unsupported field '%2'")
                    .arg(context, key));
        }
    }
    return {};
}

Expected<std::size_t, QString> sequenceLength(const sol::table &table,
                                              std::size_t maximum,
                                              std::size_t minimum,
                                              QStringView context)
{
    QSet<qsizetype> indexes;
    std::size_t entries = 0;
    for (const auto &[rawKey, _] : table)
    {
        ++entries;
        if (entries > maximum)
        {
            return makeUnexpected(
                QStringLiteral("%1 may contain at most %2 entries")
                    .arg(context)
                    .arg(maximum));
        }
        if (rawKey.get_type() != sol::type::number)
        {
            return makeUnexpected(
                QStringLiteral("%1 must be a contiguous array")
                    .arg(context));
        }
        const auto numeric = rawKey.as<double>();
        if (!std::isfinite(numeric) || std::floor(numeric) != numeric ||
            numeric < 1.0 || numeric > static_cast<double>(maximum))
        {
            return makeUnexpected(
                QStringLiteral("%1 contains an invalid array index")
                    .arg(context));
        }
        indexes.insert(static_cast<qsizetype>(numeric));
    }
    if (entries < minimum)
    {
        return makeUnexpected(
            QStringLiteral("%1 requires at least %2 entry")
                .arg(context)
                .arg(minimum));
    }
    for (std::size_t index = 1; index <= entries; ++index)
    {
        if (!indexes.contains(static_cast<qsizetype>(index)))
        {
            return makeUnexpected(
                QStringLiteral("%1 must be a contiguous array")
                    .arg(context));
        }
    }
    return entries;
}

Expected<std::optional<QString>, QString> optionalString(
    const sol::table &table, const char *key, qsizetype maxLength,
    bool allowEmpty = true)
{
    const auto value = field(table, key);
    if (value.get_type() == sol::type::none ||
        value.get_type() == sol::type::nil)
    {
        return std::optional<QString>{};
    }
    if (value.get_type() != sol::type::string)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must be a string, not %2")
                .arg(QString::fromLatin1(key), luaTypeName(value.get_type())));
    }
    auto text = value.as<QString>();
    if (!allowEmpty && text.isEmpty())
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must not be empty")
                .arg(QString::fromLatin1(key)));
    }
    if (text.size() > maxLength)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' exceeds the %2 character limit")
                .arg(QString::fromLatin1(key))
                .arg(maxLength));
    }
    return std::optional<QString>{std::move(text)};
}

Expected<QString, QString> requiredString(const sol::table &table,
                                          const char *key,
                                          qsizetype maxLength)
{
    auto value = optionalString(table, key, maxLength, false);
    if (!value)
    {
        return makeUnexpected(value.error());
    }
    if (!value->has_value())
    {
        return makeUnexpected(
            QStringLiteral("required field '%1' is missing")
                .arg(QString::fromLatin1(key)));
    }
    return std::move(**value);
}

Expected<std::optional<bool>, QString> optionalBool(const sol::table &table,
                                                    const char *key)
{
    const auto value = field(table, key);
    if (value.get_type() == sol::type::none ||
        value.get_type() == sol::type::nil)
    {
        return std::optional<bool>{};
    }
    if (value.get_type() != sol::type::boolean)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must be a boolean, not %2")
                .arg(QString::fromLatin1(key), luaTypeName(value.get_type())));
    }
    return std::optional<bool>{value.as<bool>()};
}

Expected<std::optional<double>, QString> optionalNumber(
    const sol::table &table, const char *key)
{
    const auto value = field(table, key);
    if (value.get_type() == sol::type::none ||
        value.get_type() == sol::type::nil)
    {
        return std::optional<double>{};
    }
    if (value.get_type() != sol::type::number)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must be a number, not %2")
                .arg(QString::fromLatin1(key), luaTypeName(value.get_type())));
    }
    const auto number = value.as<double>();
    if (!std::isfinite(number) || std::abs(number) > MAX_ABSOLUTE_VALUE)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must be finite and within ±%2")
                .arg(QString::fromLatin1(key))
                .arg(MAX_ABSOLUTE_VALUE, 0, 'g'));
    }
    return std::optional<double>{number};
}

Expected<std::optional<sol::table>, QString> optionalTable(
    const sol::table &table, const char *key)
{
    const auto value = field(table, key);
    if (value.get_type() == sol::type::none ||
        value.get_type() == sol::type::nil)
    {
        return std::optional<sol::table>{};
    }
    if (value.get_type() != sol::type::table)
    {
        return makeUnexpected(
            QStringLiteral("field '%1' must be a table, not %2")
                .arg(QString::fromLatin1(key), luaTypeName(value.get_type())));
    }
    return std::optional<sol::table>{value.as<sol::table>()};
}

Expected<QString, QString> parseIcon(const sol::table &table, const char *key,
                                     QString current = {})
{
    auto value = optionalString(table, key, 24);
    if (!value)
    {
        return makeUnexpected(value.error());
    }
    if (value->has_value())
    {
        current = std::move(**value);
    }
    if (!acceptedIcons().contains(current))
    {
        return makeUnexpected(
            QStringLiteral("field '%1' names unsupported built-in icon '%2'")
                .arg(QString::fromLatin1(key), current));
    }
    return current;
}

Expected<PluginControlTabSeverity, QString> parseSeverity(
    const sol::table &table, PluginControlTabSeverity current)
{
    auto raw = optionalString(table, "severity", 16);
    if (!raw)
    {
        return makeUnexpected(raw.error());
    }
    if (!raw->has_value())
    {
        return current;
    }
    const auto value = (**raw).toLower();
    if (value == QStringLiteral("neutral"))
    {
        return PluginControlTabSeverity::Neutral;
    }
    if (value == QStringLiteral("active"))
    {
        return PluginControlTabSeverity::Active;
    }
    if (value == QStringLiteral("success"))
    {
        return PluginControlTabSeverity::Success;
    }
    if (value == QStringLiteral("warning"))
    {
        return PluginControlTabSeverity::Warning;
    }
    if (value == QStringLiteral("error"))
    {
        return PluginControlTabSeverity::Error;
    }
    if (value == QStringLiteral("unknown"))
    {
        return PluginControlTabSeverity::Unknown;
    }
    return makeUnexpected(
        QStringLiteral("field 'severity' must be neutral, active, success, "
                       "warning, error, or unknown"));
}

Expected<PluginControlType, QString> parseControlType(const sol::table &table)
{
    auto raw = requiredString(table, "type", 16);
    if (!raw)
    {
        return makeUnexpected(raw.error());
    }
    const auto value = raw->toLower();
    if (value == QStringLiteral("status"))
    {
        return PluginControlType::Status;
    }
    if (value == QStringLiteral("action"))
    {
        return PluginControlType::Action;
    }
    if (value == QStringLiteral("toggle"))
    {
        return PluginControlType::Toggle;
    }
    if (value == QStringLiteral("choice"))
    {
        return PluginControlType::Choice;
    }
    return makeUnexpected(
        QStringLiteral("field 'type' must be status, action, toggle, or choice"));
}

Expected<PluginControlTabSummary, QString> parseSummary(
    const sol::table &table, PluginControlTabSummary base)
{
    static const QSet<QString> keys{
        QStringLiteral("primary_text"), QStringLiteral("secondary_text"),
        QStringLiteral("value"),        QStringLiteral("units"),
        QStringLiteral("severity"),     QStringLiteral("stale"),
        QStringLiteral("open_enabled"),
    };
    if (auto valid = validateKeys(table, keys, u"control-tab summary"); !valid)
    {
        return makeUnexpected(valid.error());
    }

    auto primary = optionalString(table, "primary_text", MAX_PRIMARY_LENGTH);
    auto secondary =
        optionalString(table, "secondary_text", MAX_SECONDARY_LENGTH);
    auto value = optionalNumber(table, "value");
    auto units = optionalString(table, "units", MAX_UNITS_LENGTH);
    auto severity = parseSeverity(table, base.severity);
    auto stale = optionalBool(table, "stale");
    auto openEnabled = optionalBool(table, "open_enabled");
    if (!primary || !secondary || !value || !units || !severity || !stale ||
        !openEnabled)
    {
        const QStringList errors{
            primary ? QString{} : primary.error(),
            secondary ? QString{} : secondary.error(),
            value ? QString{} : value.error(),
            units ? QString{} : units.error(),
            severity ? QString{} : severity.error(),
            stale ? QString{} : stale.error(),
            openEnabled ? QString{} : openEnabled.error(),
        };
        for (const auto &error : errors)
        {
            if (!error.isEmpty())
            {
                return makeUnexpected(error);
            }
        }
    }

    if (primary->has_value())
    {
        base.primaryText = std::move(**primary);
    }
    if (secondary->has_value())
    {
        base.secondaryText = std::move(**secondary);
    }
    if (hasField(table, "value"))
    {
        base.numericValue = **value;
    }
    if (units->has_value())
    {
        base.units = std::move(**units);
    }
    base.severity = *severity;
    if (stale->has_value())
    {
        base.stale = **stale;
    }
    if (openEnabled->has_value())
    {
        base.openEnabled = **openEnabled;
    }
    return base;
}

Expected<std::vector<PluginControlChoice>, QString> parseChoices(
    const sol::table &table)
{
    auto count = sequenceLength(table, MAX_CHOICES, 1, u"choice options");
    if (!count)
    {
        return makeUnexpected(count.error());
    }

    std::vector<PluginControlChoice> choices;
    choices.reserve(*count);
    QSet<QString> values;
    static const QSet<QString> keys{QStringLiteral("value"),
                                    QStringLiteral("label")};
    for (std::size_t index = 1; index <= *count; ++index)
    {
        const auto raw = table.raw_get<sol::object>(index);
        if (raw.get_type() != sol::type::table)
        {
            return makeUnexpected(
                QStringLiteral("choice option #%1 must be a table").arg(index));
        }
        const auto option = raw.as<sol::table>();
        if (auto valid = validateKeys(option, keys, u"choice option"); !valid)
        {
            return makeUnexpected(valid.error());
        }
        auto value = requiredString(option, "value", MAX_ID_LENGTH);
        auto label = requiredString(option, "label", MAX_LABEL_LENGTH);
        if (!value)
        {
            return makeUnexpected(value.error());
        }
        if (!label)
        {
            return makeUnexpected(label.error());
        }
        if (values.contains(*value))
        {
            return makeUnexpected(
                QStringLiteral("choice option value '%1' is duplicated")
                    .arg(*value));
        }
        values.insert(*value);
        choices.push_back({std::move(*value), std::move(*label)});
    }
    return choices;
}

Expected<PluginControl, QString> parseControl(const sol::table &table)
{
    static const QSet<QString> keys{
        QStringLiteral("id"),          QStringLiteral("type"),
        QStringLiteral("label"),       QStringLiteral("description"),
        QStringLiteral("icon"),        QStringLiteral("visible"),
        QStringLiteral("enabled"),     QStringLiteral("pending"),
        QStringLiteral("text"),        QStringLiteral("value"),
        QStringLiteral("units"),       QStringLiteral("progress"),
        QStringLiteral("stale"),       QStringLiteral("options"),
    };
    if (auto valid = validateKeys(table, keys, u"control descriptor"); !valid)
    {
        return makeUnexpected(valid.error());
    }

    PluginControl control;
    auto id = requiredString(table, "id", MAX_ID_LENGTH);
    auto type = parseControlType(table);
    auto label = requiredString(table, "label", MAX_LABEL_LENGTH);
    auto description =
        optionalString(table, "description", MAX_DESCRIPTION_LENGTH);
    auto icon = parseIcon(table, "icon");
    auto visible = optionalBool(table, "visible");
    auto enabled = optionalBool(table, "enabled");
    auto pending = optionalBool(table, "pending");
    if (!id || !type || !label || !description || !icon || !visible ||
        !enabled || !pending)
    {
        if (!id)
        {
            return makeUnexpected(id.error());
        }
        if (!type)
        {
            return makeUnexpected(type.error());
        }
        if (!label)
        {
            return makeUnexpected(label.error());
        }
        if (!description)
        {
            return makeUnexpected(description.error());
        }
        if (!icon)
        {
            return makeUnexpected(icon.error());
        }
        if (!visible)
        {
            return makeUnexpected(visible.error());
        }
        if (!enabled)
        {
            return makeUnexpected(enabled.error());
        }
        return makeUnexpected(pending.error());
    }
    control.id = std::move(*id);
    control.type = *type;
    control.label = std::move(*label);
    if (description->has_value())
    {
        control.description = std::move(**description);
    }
    control.icon = std::move(*icon);
    if (visible->has_value())
    {
        control.visible = **visible;
    }
    if (enabled->has_value())
    {
        control.enabled = **enabled;
    }
    if (pending->has_value())
    {
        control.pending = **pending;
    }

    switch (control.type)
    {
        case PluginControlType::Status: {
            if (hasField(table, "options"))
            {
                return makeUnexpected(
                    QStringLiteral("status controls do not accept options"));
            }
            auto text = optionalString(table, "text", MAX_SECONDARY_LENGTH);
            auto value = optionalNumber(table, "value");
            auto units = optionalString(table, "units", MAX_UNITS_LENGTH);
            auto progress = optionalNumber(table, "progress");
            auto stale = optionalBool(table, "stale");
            if (!text || !value || !units || !progress || !stale)
            {
                if (!text)
                {
                    return makeUnexpected(text.error());
                }
                if (!value)
                {
                    return makeUnexpected(value.error());
                }
                if (!units)
                {
                    return makeUnexpected(units.error());
                }
                if (!progress)
                {
                    return makeUnexpected(progress.error());
                }
                return makeUnexpected(stale.error());
            }
            if (progress->has_value() &&
                (**progress < 0.0 || **progress > 1.0))
            {
                return makeUnexpected(QStringLiteral(
                    "status progress must be between 0 and 1"));
            }
            if (text->has_value())
            {
                control.text = std::move(**text);
            }
            if (value->has_value())
            {
                control.numericValue = **value;
            }
            if (units->has_value())
            {
                control.units = std::move(**units);
            }
            if (progress->has_value())
            {
                control.progress = **progress;
            }
            if (stale->has_value())
            {
                control.stale = **stale;
            }
            break;
        }
        case PluginControlType::Action:
            if (hasField(table, "value") || hasField(table, "options") ||
                hasField(table, "progress") || hasField(table, "text") ||
                hasField(table, "units") || hasField(table, "stale"))
            {
                return makeUnexpected(QStringLiteral(
                    "action controls do not accept status or value fields"));
            }
            break;
        case PluginControlType::Toggle: {
            if (!hasField(table, "value"))
            {
                return makeUnexpected(
                    QStringLiteral("toggle controls require boolean value"));
            }
            auto value = optionalBool(table, "value");
            if (!value)
            {
                return makeUnexpected(value.error());
            }
            control.toggleValue = **value;
            if (hasField(table, "options") || hasField(table, "progress") ||
                hasField(table, "text") || hasField(table, "units") ||
                hasField(table, "stale"))
            {
                return makeUnexpected(QStringLiteral(
                    "toggle controls do not accept status or option fields"));
            }
            break;
        }
        case PluginControlType::Choice: {
            auto value = requiredString(table, "value", MAX_ID_LENGTH);
            auto options = optionalTable(table, "options");
            if (!value)
            {
                return makeUnexpected(value.error());
            }
            if (!options)
            {
                return makeUnexpected(options.error());
            }
            if (!options->has_value())
            {
                return makeUnexpected(
                    QStringLiteral("choice controls require options"));
            }
            auto choices = parseChoices(**options);
            if (!choices)
            {
                return makeUnexpected(choices.error());
            }
            if (std::ranges::none_of(*choices, [&](const auto &choice) {
                    return choice.value == *value;
                }))
            {
                return makeUnexpected(QStringLiteral(
                    "choice value must match one declared option"));
            }
            control.choiceValue = std::move(*value);
            control.choices = std::move(*choices);
            if (hasField(table, "progress") || hasField(table, "text") ||
                hasField(table, "units") || hasField(table, "stale"))
            {
                return makeUnexpected(QStringLiteral(
                    "choice controls do not accept status fields"));
            }
            break;
        }
    }
    return control;
}

Expected<std::vector<PluginControl>, QString> parseControls(
    const sol::table &table)
{
    auto count = sequenceLength(table, MAX_CONTROLS, 0, u"control list");
    if (!count)
    {
        return makeUnexpected(count.error());
    }
    std::vector<PluginControl> controls;
    controls.reserve(*count);
    QSet<QString> ids;
    for (std::size_t index = 1; index <= *count; ++index)
    {
        const auto raw = table.raw_get<sol::object>(index);
        if (raw.get_type() != sol::type::table)
        {
            return makeUnexpected(
                QStringLiteral("control #%1 must be a table").arg(index));
        }
        auto control = parseControl(raw.as<sol::table>());
        if (!control)
        {
            return makeUnexpected(
                QStringLiteral("control #%1: %2").arg(index).arg(control.error()));
        }
        if (ids.contains(control->id))
        {
            return makeUnexpected(
                QStringLiteral("control id '%1' is duplicated").arg(control->id));
        }
        ids.insert(control->id);
        controls.push_back(std::move(*control));
    }
    return controls;
}

Expected<PluginControlTabSnapshot, QString> parseDescriptor(
    Plugin *plugin, const sol::table &table)
{
    static const QSet<QString> keys{
        QStringLiteral("title"),            QStringLiteral("icon"),
        QStringLiteral("tooltip"),          QStringLiteral("accessible_label"),
        QStringLiteral("visible"),          QStringLiteral("summary"),
        QStringLiteral("controls"),         QStringLiteral("on_control"),
    };
    if (auto valid = validateKeys(table, keys, u"control-tab descriptor");
        !valid)
    {
        return makeUnexpected(valid.error());
    }

    PluginControlTabSnapshot snapshot;
    snapshot.pluginId = plugin->id;
    snapshot.pluginName = plugin->meta.name;
    auto title = requiredString(table, "title", MAX_TITLE_LENGTH);
    auto icon = parseIcon(table, "icon");
    auto tooltip = optionalString(table, "tooltip", MAX_TOOLTIP_LENGTH);
    auto accessible =
        optionalString(table, "accessible_label", MAX_ACCESSIBLE_LENGTH);
    auto visible = optionalBool(table, "visible");
    auto summaryTable = optionalTable(table, "summary");
    auto controlsTable = optionalTable(table, "controls");
    if (!title || !icon || !tooltip || !accessible || !visible ||
        !summaryTable || !controlsTable)
    {
        if (!title)
        {
            return makeUnexpected(title.error());
        }
        if (!icon)
        {
            return makeUnexpected(icon.error());
        }
        if (!tooltip)
        {
            return makeUnexpected(tooltip.error());
        }
        if (!accessible)
        {
            return makeUnexpected(accessible.error());
        }
        if (!visible)
        {
            return makeUnexpected(visible.error());
        }
        if (!summaryTable)
        {
            return makeUnexpected(summaryTable.error());
        }
        return makeUnexpected(controlsTable.error());
    }
    snapshot.title = std::move(*title);
    snapshot.icon = std::move(*icon);
    if (tooltip->has_value())
    {
        snapshot.tooltip = std::move(**tooltip);
    }
    if (accessible->has_value())
    {
        snapshot.accessibleLabel = std::move(**accessible);
    }
    if (visible->has_value())
    {
        snapshot.visible = **visible;
    }
    if (summaryTable->has_value())
    {
        auto summary = parseSummary(**summaryTable, {});
        if (!summary)
        {
            return makeUnexpected(summary.error());
        }
        snapshot.summary = std::move(*summary);
    }
    if (controlsTable->has_value())
    {
        auto controls = parseControls(**controlsTable);
        if (!controls)
        {
            return makeUnexpected(controls.error());
        }
        snapshot.controls = std::move(*controls);
    }
    return snapshot;
}

Expected<void, QString> applyControlUpdate(PluginControl &control,
                                           const sol::table &table)
{
    static const QSet<QString> keys{
        QStringLiteral("id"),          QStringLiteral("visible"),
        QStringLiteral("enabled"),     QStringLiteral("pending"),
        QStringLiteral("text"),        QStringLiteral("value"),
        QStringLiteral("units"),       QStringLiteral("progress"),
        QStringLiteral("stale"),       QStringLiteral("options"),
        QStringLiteral("clear_value"), QStringLiteral("clear_progress"),
    };
    if (auto valid = validateKeys(table, keys, u"control state update"); !valid)
    {
        return makeUnexpected(valid.error());
    }

    auto visible = optionalBool(table, "visible");
    auto enabled = optionalBool(table, "enabled");
    auto pending = optionalBool(table, "pending");
    if (!visible || !enabled || !pending)
    {
        if (!visible)
        {
            return makeUnexpected(visible.error());
        }
        if (!enabled)
        {
            return makeUnexpected(enabled.error());
        }
        return makeUnexpected(pending.error());
    }
    if (visible->has_value())
    {
        control.visible = **visible;
    }
    if (enabled->has_value())
    {
        control.enabled = **enabled;
    }
    if (pending->has_value())
    {
        control.pending = **pending;
    }

    switch (control.type)
    {
        case PluginControlType::Status: {
            auto text = optionalString(table, "text", MAX_SECONDARY_LENGTH);
            auto value = optionalNumber(table, "value");
            auto units = optionalString(table, "units", MAX_UNITS_LENGTH);
            auto progress = optionalNumber(table, "progress");
            auto stale = optionalBool(table, "stale");
            auto clearValue = optionalBool(table, "clear_value");
            auto clearProgress = optionalBool(table, "clear_progress");
            if (!text || !value || !units || !progress || !stale ||
                !clearValue || !clearProgress)
            {
                if (!text)
                {
                    return makeUnexpected(text.error());
                }
                if (!value)
                {
                    return makeUnexpected(value.error());
                }
                if (!units)
                {
                    return makeUnexpected(units.error());
                }
                if (!progress)
                {
                    return makeUnexpected(progress.error());
                }
                if (!stale)
                {
                    return makeUnexpected(stale.error());
                }
                if (!clearValue)
                {
                    return makeUnexpected(clearValue.error());
                }
                return makeUnexpected(clearProgress.error());
            }
            if (progress->has_value() &&
                (**progress < 0.0 || **progress > 1.0))
            {
                return makeUnexpected(QStringLiteral(
                    "status progress must be between 0 and 1"));
            }
            if (text->has_value())
            {
                control.text = std::move(**text);
            }
            if (clearValue->value_or(false))
            {
                control.numericValue.reset();
            }
            if (value->has_value())
            {
                control.numericValue = **value;
            }
            if (units->has_value())
            {
                control.units = std::move(**units);
            }
            if (clearProgress->value_or(false))
            {
                control.progress.reset();
            }
            if (progress->has_value())
            {
                control.progress = **progress;
            }
            if (stale->has_value())
            {
                control.stale = **stale;
            }
            if (hasField(table, "options"))
            {
                return makeUnexpected(
                    QStringLiteral("status controls do not accept options"));
            }
            break;
        }
        case PluginControlType::Action:
            if (hasField(table, "value") || hasField(table, "options") ||
                hasField(table, "progress") || hasField(table, "text") ||
                hasField(table, "units") || hasField(table, "stale") ||
                hasField(table, "clear_value") ||
                hasField(table, "clear_progress"))
            {
                return makeUnexpected(
                    QStringLiteral("action state only supports common fields"));
            }
            break;
        case PluginControlType::Toggle: {
            if (hasField(table, "value"))
            {
                auto value = optionalBool(table, "value");
                if (!value)
                {
                    return makeUnexpected(value.error());
                }
                control.toggleValue = **value;
            }
            if (hasField(table, "options") || hasField(table, "progress") ||
                hasField(table, "text") || hasField(table, "units") ||
                hasField(table, "stale") || hasField(table, "clear_value") ||
                hasField(table, "clear_progress"))
            {
                return makeUnexpected(
                    QStringLiteral("toggle state only supports value and common fields"));
            }
            break;
        }
        case PluginControlType::Choice: {
            if (hasField(table, "options"))
            {
                auto options = optionalTable(table, "options");
                if (!options)
                {
                    return makeUnexpected(options.error());
                }
                auto choices = parseChoices(**options);
                if (!choices)
                {
                    return makeUnexpected(choices.error());
                }
                control.choices = std::move(*choices);
            }
            if (hasField(table, "value"))
            {
                auto value = requiredString(table, "value", MAX_ID_LENGTH);
                if (!value)
                {
                    return makeUnexpected(value.error());
                }
                control.choiceValue = std::move(*value);
            }
            if (std::ranges::none_of(control.choices, [&](const auto &choice) {
                    return choice.value == control.choiceValue;
                }))
            {
                return makeUnexpected(QStringLiteral(
                    "choice value must match one declared option"));
            }
            if (hasField(table, "progress") || hasField(table, "text") ||
                hasField(table, "units") || hasField(table, "stale") ||
                hasField(table, "clear_value") ||
                hasField(table, "clear_progress"))
            {
                return makeUnexpected(
                    QStringLiteral("choice state does not accept status fields"));
            }
            break;
        }
    }
    return {};
}

[[noreturn]] void failLua(sol::this_state state, const QString &error)
{
    const auto utf8 = error.toUtf8();
    lua::fail(state.lua_state(), "%s", utf8.constData());
}

}  // namespace

class PluginControlTabRegistration::Impl
{
public:
    Impl(Plugin *owner, PluginControlTabSnapshot snapshot,
         std::optional<sol::main_protected_function> callback)
        : owner(owner)
        , snapshot(std::move(snapshot))
        , callback(std::move(callback))
    {
    }

    Plugin *owner = nullptr;
    PluginControlTabSnapshot snapshot;
    std::optional<sol::main_protected_function> callback;
    bool active = true;
    bool callbackActive = false;
};

PluginControlTabRegistration::PluginControlTabRegistration(
    Plugin *owner, PluginControlTabSnapshot snapshot,
    std::optional<sol::main_protected_function> callback)
    : impl_(std::make_unique<Impl>(owner, std::move(snapshot),
                                  std::move(callback)))
{
}

PluginControlTabRegistration::~PluginControlTabRegistration() = default;

const PluginControlTabSnapshot &PluginControlTabRegistration::snapshot() const
{
    return this->impl_->snapshot;
}

bool PluginControlTabRegistration::isActive() const
{
    return this->impl_->active;
}

Expected<void, QString> PluginControlTabRegistration::updateSummary(
    const sol::table &state)
{
    if (!this->impl_->active)
    {
        return makeUnexpected(QStringLiteral("control-tab handle is stale"));
    }
    auto next = parseSummary(state, {});
    if (!next)
    {
        return makeUnexpected(next.error());
    }
    this->impl_->snapshot.summary = std::move(*next);
    getApp()->getPlugins()->queueControlTabUpdate();
    return {};
}

Expected<void, QString> PluginControlTabRegistration::updateControls(
    const sol::table &states)
{
    if (!this->impl_->active)
    {
        return makeUnexpected(QStringLiteral("control-tab handle is stale"));
    }
    auto count = sequenceLength(states, MAX_CONTROLS, 0,
                                u"control update list");
    if (!count)
    {
        return makeUnexpected(count.error());
    }

    auto next = this->impl_->snapshot.controls;
    QSet<QString> updated;
    for (std::size_t index = 1; index <= *count; ++index)
    {
        const auto raw = states.raw_get<sol::object>(index);
        if (raw.get_type() != sol::type::table)
        {
            return makeUnexpected(
                QStringLiteral("control update #%1 must be a table").arg(index));
        }
        const auto update = raw.as<sol::table>();
        auto id = requiredString(update, "id", MAX_ID_LENGTH);
        if (!id)
        {
            return makeUnexpected(
                QStringLiteral("control update #%1: %2")
                    .arg(index)
                    .arg(id.error()));
        }
        if (updated.contains(*id))
        {
            return makeUnexpected(
                QStringLiteral("control update id '%1' is duplicated").arg(*id));
        }
        updated.insert(*id);
        auto it = std::ranges::find(next, *id, &PluginControl::id);
        if (it == next.end())
        {
            return makeUnexpected(
                QStringLiteral("control update names unknown id '%1'").arg(*id));
        }
        if (auto applied = applyControlUpdate(*it, update); !applied)
        {
            return makeUnexpected(
                QStringLiteral("control '%1': %2").arg(*id, applied.error()));
        }
    }
    this->impl_->snapshot.controls = std::move(next);
    getApp()->getPlugins()->queueControlTabUpdate();
    return {};
}

Expected<void, QString> PluginControlTabRegistration::setVisible(bool visible)
{
    if (!this->impl_->active)
    {
        return makeUnexpected(QStringLiteral("control-tab handle is stale"));
    }
    if (this->impl_->snapshot.visible == visible)
    {
        return {};
    }
    this->impl_->snapshot.visible = visible;
    getApp()->getPlugins()->notifyControlTabsChanged();
    return {};
}

Expected<void, QString> PluginControlTabRegistration::remove()
{
    if (!this->impl_->active)
    {
        return makeUnexpected(QStringLiteral("control-tab handle is stale"));
    }
    auto *owner = this->impl_->owner;
    this->impl_->active = false;
    this->impl_->owner = nullptr;
    this->impl_->callback.reset();
    owner->removeControlTab(this);
    getApp()->getPlugins()->notifyControlTabsChanged();
    return {};
}

Expected<void, QString> PluginControlTabRegistration::invoke(
    const QString &controlID, const PluginControlValue &proposedValue)
{
    if (!this->impl_->active || this->impl_->owner == nullptr)
    {
        return makeUnexpected(QStringLiteral("control-tab handle is stale"));
    }
    if (this->impl_->callbackActive)
    {
        return makeUnexpected(
            QStringLiteral("control callback re-entry is not allowed"));
    }
    auto control = std::ranges::find(this->impl_->snapshot.controls, controlID,
                                     &PluginControl::id);
    if (control == this->impl_->snapshot.controls.end())
    {
        return makeUnexpected(
            QStringLiteral("unknown control id '%1'").arg(controlID));
    }
    if (!control->visible || !control->enabled || control->pending)
    {
        return makeUnexpected(
            QStringLiteral("control '%1' is not currently interactive")
                .arg(controlID));
    }
    if (control->type == PluginControlType::Status)
    {
        return makeUnexpected(
            QStringLiteral("status controls are read-only"));
    }
    if (!this->impl_->callback.has_value())
    {
        return makeUnexpected(
            QStringLiteral("this control tab has no control callback"));
    }

    if ((control->type == PluginControlType::Action &&
         !std::holds_alternative<std::monostate>(proposedValue)) ||
        (control->type == PluginControlType::Toggle &&
         !std::holds_alternative<bool>(proposedValue)) ||
        (control->type == PluginControlType::Choice &&
         !std::holds_alternative<QString>(proposedValue)))
    {
        return makeUnexpected(
            QStringLiteral("proposed value has the wrong type for control '%1'")
                .arg(controlID));
    }
    if (control->type == PluginControlType::Choice)
    {
        const auto &value = std::get<QString>(proposedValue);
        if (std::ranges::none_of(control->choices, [&](const auto &choice) {
                return choice.value == value;
            }))
        {
            return makeUnexpected(
                QStringLiteral("proposed choice '%1' is not an available option")
                    .arg(value));
        }
    }

    const auto before = this->impl_->snapshot;
    auto *const callbackOwner = this->impl_->owner;
    this->impl_->callbackActive = true;
    const auto reset = qScopeGuard([this] {
        this->impl_->callbackActive = false;
    });

    Expected<void, QString> result;
    if (control->type == PluginControlType::Action)
    {
        result = lua::tryCall<void>(*this->impl_->callback, controlID);
    }
    else if (control->type == PluginControlType::Toggle)
    {
        result = lua::tryCall<void>(*this->impl_->callback, controlID,
                                    std::get<bool>(proposedValue));
    }
    else
    {
        result = lua::tryCall<void>(*this->impl_->callback, controlID,
                                    std::get<QString>(proposedValue));
    }

    if (!result)
    {
        // A callback may remove its own tab. Removal wins and must not be
        // undone. Otherwise roll back any state it published before failing.
        if (this->impl_->active)
        {
            this->impl_->snapshot = before;
            getApp()->getPlugins()->notifyControlTabsChanged();
        }
        lua::logError(callbackOwner, u"control-tab callback", result.error());
        return makeUnexpected(result.error());
    }
    return {};
}

void PluginControlTabRegistration::detach()
{
    if (!this->impl_->active)
    {
        return;
    }
    this->impl_->active = false;
    this->impl_->owner = nullptr;
    this->impl_->callback.reset();
}

PluginControlTabHandle::PluginControlTabHandle(
    const std::shared_ptr<PluginControlTabRegistration> &registration)
    : registration_(registration)
{
}

void PluginControlTabHandle::update_summary(sol::this_state state,
                                            const sol::table &summary)
{
    auto registration = this->registration_.lock();
    if (!registration)
    {
        failLua(state, QStringLiteral("control-tab handle is stale"));
    }
    if (auto result = registration->updateSummary(summary); !result)
    {
        failLua(state, result.error());
    }
}

void PluginControlTabHandle::update_controls(sol::this_state state,
                                             const sol::table &controls)
{
    auto registration = this->registration_.lock();
    if (!registration)
    {
        failLua(state, QStringLiteral("control-tab handle is stale"));
    }
    if (auto result = registration->updateControls(controls); !result)
    {
        failLua(state, result.error());
    }
}

void PluginControlTabHandle::set_visible(sol::this_state state, bool visible)
{
    auto registration = this->registration_.lock();
    if (!registration)
    {
        failLua(state, QStringLiteral("control-tab handle is stale"));
    }
    if (auto result = registration->setVisible(visible); !result)
    {
        failLua(state, result.error());
    }
}

void PluginControlTabHandle::remove(sol::this_state state)
{
    auto registration = this->registration_.lock();
    if (!registration)
    {
        failLua(state, QStringLiteral("control-tab handle is stale"));
    }
    if (auto result = registration->remove(); !result)
    {
        failLua(state, result.error());
    }
}

bool PluginControlTabHandle::is_valid() const
{
    auto registration = this->registration_.lock();
    return registration && registration->isActive();
}

void createControlTabUserTypes(sol::table &c2, Plugin *plugin)
{
    c2.new_usertype<PluginControlTabHandle>(
        "PluginControlTabHandle", sol::no_constructor,
        "update_summary", &PluginControlTabHandle::update_summary,
        "update_controls", &PluginControlTabHandle::update_controls,
        "set_visible", &PluginControlTabHandle::set_visible,
        "remove", &PluginControlTabHandle::remove,
        "is_valid", &PluginControlTabHandle::is_valid);

    sol::state_view lua(c2.lua_state());
    auto ui = lua.create_table();
    ui.set_function(
        "register_control_tab",
        [plugin](sol::this_state state,
                 const sol::table &descriptor) -> PluginControlTabHandle {
            if (!plugin->hasPermission(
                    PluginPermission::Type::UiControlTabs))
            {
                failLua(state,
                        QStringLiteral("register_control_tab requires the "
                                       "UiControlTabs permission"));
            }
            if (plugin->controlTab())
            {
                failLua(state, QStringLiteral(
                                   "this plugin generation already owns a "
                                   "control tab"));
            }
            if (getApp()->getPlugins()->activeControlTabCount() >=
                PluginController::MAX_CONTROL_TABS)
            {
                failLua(state, QStringLiteral(
                                   "the application-wide control-tab limit "
                                   "has been reached"));
            }

            auto snapshot = parseDescriptor(plugin, descriptor);
            if (!snapshot)
            {
                failLua(state, snapshot.error());
            }

            std::optional<sol::main_protected_function> callback;
            const auto rawCallback = field(descriptor, "on_control");
            if (rawCallback.get_type() != sol::type::none &&
                rawCallback.get_type() != sol::type::nil)
            {
                if (rawCallback.get_type() != sol::type::function)
                {
                    failLua(state,
                            QStringLiteral("field 'on_control' must be a "
                                           "function"));
                }
                callback =
                    rawCallback.as<sol::main_protected_function>();
            }
            const bool hasInteractive = std::ranges::any_of(
                snapshot->controls, [](const auto &control) {
                    return control.type != PluginControlType::Status;
                });
            if (hasInteractive && !callback.has_value())
            {
                failLua(state,
                        QStringLiteral("interactive controls require an "
                                       "on_control callback"));
            }

            auto registration =
                std::make_shared<PluginControlTabRegistration>(
                    plugin, std::move(*snapshot), std::move(callback));
            plugin->setControlTab(registration);
            getApp()->getPlugins()->notifyControlTabsChanged();
            return PluginControlTabHandle(registration);
        });
    c2["ui"] = std::move(ui);
}

}  // namespace chatterino::lua::api
#endif
