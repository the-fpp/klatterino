// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "util/Expected.hpp"

#    include <QString>
#    include <sol/forward.hpp>

#    include <memory>
#    include <optional>
#    include <variant>
#    include <vector>

namespace chatterino {

class Plugin;
class PluginController;

enum class PluginControlTabSeverity {
    Neutral,
    Active,
    Success,
    Warning,
    Error,
    Unknown,
};

enum class PluginControlType {
    Status,
    Action,
    Toggle,
    Choice,
};

struct PluginControlChoice {
    QString value;
    QString label;

    bool operator==(const PluginControlChoice &) const = default;
};

struct PluginControl {
    QString id;
    PluginControlType type = PluginControlType::Status;
    QString label;
    QString description;
    QString icon;
    bool visible = true;
    bool enabled = true;
    bool pending = false;

    // Status fields.
    QString text;
    std::optional<double> numericValue;
    QString units;
    std::optional<double> progress;
    bool stale = false;

    // Toggle and choice fields.
    bool toggleValue = false;
    QString choiceValue;
    std::vector<PluginControlChoice> choices;

    bool operator==(const PluginControl &) const = default;
};

struct PluginControlTabSummary {
    QString primaryText;
    QString secondaryText;
    std::optional<double> numericValue;
    QString units;
    PluginControlTabSeverity severity = PluginControlTabSeverity::Neutral;
    bool stale = false;
    bool openEnabled = true;

    bool operator==(const PluginControlTabSummary &) const = default;
};

struct PluginControlTabSnapshot {
    QString pluginId;
    QString pluginName;
    QString title;
    QString icon;
    QString tooltip;
    QString accessibleLabel;
    PluginControlTabSummary summary;
    std::vector<PluginControl> controls;
    bool visible = true;

    bool operator==(const PluginControlTabSnapshot &) const = default;
};

using PluginControlValue = std::variant<std::monostate, bool, QString>;

namespace lua::api {

/* @lua-fragment

---@alias c2.PluginControlTabSeverity 'neutral'|'active'|'success'|'warning'|'error'|'unknown'
---@alias c2.PluginControlIcon ''|'play'|'pause'|'stop'|'refresh'|'settings'|'info'|'warning'|'error'|'check'|'status'

---@class c2.PluginControlTabSummary
---@field primary_text? string
---@field secondary_text? string
---@field value? number
---@field units? string
---@field severity? c2.PluginControlTabSeverity
---@field stale? boolean
---@field open_enabled? boolean

---@class c2.PluginControlChoiceOption
---@field value string
---@field label string

---@class c2.PluginControl
---@field id string Stable plugin-local ID.
---@field type 'status'|'action'|'toggle'|'choice'
---@field label string
---@field description? string
---@field icon? c2.PluginControlIcon
---@field visible? boolean
---@field enabled? boolean
---@field pending? boolean
---@field text? string Status text.
---@field value? number|boolean|string Numeric status, toggle, or selected choice value.
---@field units? string Status units.
---@field progress? number Status progress from 0 through 1.
---@field stale? boolean Status freshness.
---@field options? c2.PluginControlChoiceOption[] Choice options.

---@class c2.PluginControlStateUpdate
---@field id string Existing control ID.
---@field visible? boolean
---@field enabled? boolean
---@field pending? boolean
---@field text? string
---@field value? number|boolean|string
---@field units? string
---@field progress? number
---@field stale? boolean
---@field options? c2.PluginControlChoiceOption[]
---@field clear_value? boolean Clear a status numeric value.
---@field clear_progress? boolean Clear status progress.

---@class c2.PluginControlTabDescriptor
---@field title string
---@field icon? c2.PluginControlIcon Built-in icon name; paths, URLs, and image payloads are rejected.
---@field tooltip? string
---@field accessible_label? string
---@field visible? boolean
---@field summary? c2.PluginControlTabSummary
---@field controls? c2.PluginControl[]
---@field on_control? fun(id: string, proposed_value: boolean|string|nil) Required when interactive controls exist.

c2.ui = {}
*/

class PluginControlTabRegistration
{
public:
    class Impl;

    PluginControlTabRegistration(Plugin *owner,
                                 PluginControlTabSnapshot snapshot,
                                 std::optional<sol::main_protected_function>
                                     callback);
    ~PluginControlTabRegistration();

    PluginControlTabRegistration(const PluginControlTabRegistration &) = delete;
    PluginControlTabRegistration(PluginControlTabRegistration &&) = delete;
    PluginControlTabRegistration &operator=(
        const PluginControlTabRegistration &) = delete;
    PluginControlTabRegistration &operator=(PluginControlTabRegistration &&) =
        delete;

    const PluginControlTabSnapshot &snapshot() const;
    bool isActive() const;

    Expected<void, QString> updateSummary(const sol::table &state);
    Expected<void, QString> updateControls(const sol::table &states);
    Expected<void, QString> setVisible(bool visible);
    Expected<void, QString> remove();
    Expected<void, QString> invoke(const QString &controlID,
                                   const PluginControlValue &proposedValue);

    /// Invalidates handles and releases the Lua callback while the state is
    /// still alive. This does not emit because PluginController owns the
    /// enclosing unload/reload notification.
    void detach();

private:
    std::unique_ptr<Impl> impl_;
};

/**
 * @lua@class c2.PluginControlTabHandle
 */
class PluginControlTabHandle
{
public:
    PluginControlTabHandle() = default;
    explicit PluginControlTabHandle(
        const std::shared_ptr<PluginControlTabRegistration> &registration);

    /**
     * Atomically replaces the collapsed summary. Invalid updates preserve the last valid state.
     *
     * @lua@param summary c2.PluginControlTabSummary
     * @exposed c2.PluginControlTabHandle:update_summary
     */
    void update_summary(sol::this_state state, const sol::table &summary);
    /**
     * Atomically updates authoritative state for the named controls.
     *
     * @lua@param controls c2.PluginControlStateUpdate[]
     * @exposed c2.PluginControlTabHandle:update_controls
     */
    void update_controls(sol::this_state state, const sol::table &controls);
    /**
     * Shows or hides this plugin generation's contribution.
     *
     * @lua@param visible boolean
     * @exposed c2.PluginControlTabHandle:set_visible
     */
    void set_visible(sol::this_state state, bool visible);
    /**
     * Removes this contribution and permanently invalidates the handle.
     *
     * @exposed c2.PluginControlTabHandle:remove
     */
    void remove(sol::this_state state);
    /**
     * @lua@return boolean valid
     * @exposed c2.PluginControlTabHandle:is_valid
     */
    bool is_valid() const;

private:
    std::weak_ptr<PluginControlTabRegistration> registration_;
};

/**
 * Registers one bounded top control tab for this plugin generation.
 * Requires the `UiControlTabs` permission.
 *
 * @lua@param descriptor c2.PluginControlTabDescriptor
 * @lua@return c2.PluginControlTabHandle handle
 * @exposed c2.ui.register_control_tab
 */
void createControlTabUserTypes(sol::table &c2, Plugin *plugin);

}  // namespace lua::api
}  // namespace chatterino

#endif
