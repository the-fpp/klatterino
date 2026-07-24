// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "common/websockets/WebSocketPool.hpp"
#    include "controllers/commands/CommandContext.hpp"
#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/PluginControlTab.hpp"

#    include <pajlada/signals/signal.hpp>
#    include <QDir>
#    include <QFileInfo>
#    include <QJsonArray>
#    include <QJsonObject>
#    include <QString>
#    include <sol/forward.hpp>

#    include <map>
#    include <memory>
#    include <cstddef>
#    include <utility>
#    include <vector>

struct lua_State;

namespace chatterino {

class Settings;
class Paths;
class Split;

class PluginController
{
    const Paths &paths;

public:
    static constexpr std::size_t MAX_CONTROL_TABS = 12;

    explicit PluginController(const Paths &paths_);

    void initialize(Settings &settings);

    QString tryExecPluginCommand(const QString &commandName,
                                 const CommandContext &ctx);

    bool tryHandleEmptyInputBackspace(Split &split) const;

    // NOTE: this pointer does not own the Plugin, unique_ptr still owns it
    // This is required to be public because of c functions
    Plugin *getPluginByStatePtr(lua_State *L);

    // TODO: make a function that iterates plugins that aren't errored/enabled
    const std::map<QString, std::unique_ptr<Plugin>> &plugins() const;

    /**
     * @brief Reload plugin given by id
     *
     * @param id This is the unique identifier of the plugin, the name of the directory it is in
     */
    bool reload(const QString &id);

    /**
     * @brief Checks settings to tell if a plugin named by id is enabled.
     *
     * It is the callers responsibility to check Settings::pluginsEnabled
     */
    static bool isPluginEnabled(const QString &id);

    std::pair<bool, QStringList> updateCustomCompletions(
        const QString &query, const QString &fullTextContent,
        int cursorPosition, bool isFirstWord) const;

    WebSocketPool &webSocketPool();

    std::vector<PluginControlTabSnapshot> controlTabs() const;
    std::size_t activeControlTabCount() const;
    Expected<void, QString> invokeControlTab(
        const QString &pluginID, const QString &controlID,
        const PluginControlValue &proposedValue);

    /// Structural changes are emitted synchronously so removed plugin UI
    /// cannot remain interactive for another event-loop turn.
    void notifyControlTabsChanged();
    /// State-only changes are coalesced to one render per event-loop turn.
    void queueControlTabUpdate();

    pajlada::Signals::Signal<Plugin *> onPluginLoaded;
    pajlada::Signals::NoArgSignal onPluginsUpdated;
    pajlada::Signals::NoArgSignal onControlTabsUpdated;

private:
    void loadPlugins();
    void load(const QFileInfo &index, const QDir &pluginDir,
              const PluginMeta &meta);

    // This function adds lua standard libraries into the state
    void openLibrariesFor(Plugin *plugin);

    void initSol(sol::state_view &lua, Plugin *plugin);

    static void loadChatterinoLib(lua_State *l);
    bool tryLoadFromDir(const QDir &pluginDir);

    void queueChangeNotification();

    std::map<QString, std::unique_ptr<Plugin>> plugins_;
    WebSocketPool webSocketPool_;

    std::vector<
        std::pair<std::string, std::function<sol::object(sol::state_view)>>>
        loaders_;

    bool changeNotificationQueued = false;
    bool controlTabUpdateQueued_ = false;

    // This is for tests, pay no attention
    friend class PluginControllerAccess;
};

}  // namespace chatterino
#endif
