// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "Application.hpp"
#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/PluginControlTab.hpp"
#    include "controllers/plugins/PluginController.hpp"
#    include "controllers/plugins/PluginPermission.hpp"
#    include "mocks/BaseApplication.hpp"
#    include "singletons/WindowManager.hpp"
#    include "widgets/PluginControlTabHost.hpp"

#    include <lauxlib.h>
#    include <QApplication>
#    include <QCheckBox>
#    include <QComboBox>
#    include <QCoreApplication>
#    include <QFile>
#    include <QKeyEvent>
#    include <QLineEdit>
#    include <QMouseEvent>
#    include <QProgressBar>
#    include <QPushButton>
#    include <QToolButton>
#    include <sol/sol.hpp>

#    include <memory>
#    include <vector>

using namespace chatterino;

namespace {

class MockApplication final : public mock::BaseApplication
{
public:
    MockApplication()
        : plugins(this->paths_)
        , windows(this->args_, this->paths_, this->settings, this->theme,
                  this->fonts)
    {
    }

    PluginController *getPlugins() override
    {
        return &this->plugins;
    }

    WindowManager *getWindows() override
    {
        return &this->windows;
    }

    PluginController plugins;
    WindowManager windows;
};

PluginPermission controlTabPermission()
{
    return PluginPermission{{{"type", "UiControlTabs"}}};
}

PluginMeta metadata(QString name, bool permitted)
{
    PluginMeta meta;
    meta.name = std::move(name);
    meta.license = QStringLiteral("MIT");
    meta.description = QStringLiteral("Control-tab test plugin");
    if (permitted)
    {
        meta.permissions.push_back(controlTabPermission());
    }
    return meta;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    return file.write(contents) == contents.size();
}

}  // namespace

namespace chatterino {

class PluginControllerAccess
{
public:
    static bool tryLoadFromDir(const QDir &pluginDir)
    {
        return getApp()->getPlugins()->tryLoadFromDir(pluginDir);
    }

    static void openLibrariesFor(Plugin *plugin)
    {
        getApp()->getPlugins()->openLibrariesFor(plugin);
    }

    static std::map<QString, std::unique_ptr<Plugin>> &plugins()
    {
        return getApp()->getPlugins()->plugins_;
    }

    static lua_State *state(Plugin *plugin)
    {
        return plugin->state_;
    }
};

}  // namespace chatterino

class PluginControlTabTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        this->app = std::make_unique<MockApplication>();
    }

    void TearDown() override
    {
        this->states.clear();
        PluginControllerAccess::plugins().clear();
        this->app.reset();
    }

    sol::state_view &addPlugin(const QString &id, const QString &name,
                               bool permitted = true)
    {
        auto plugin = std::make_unique<Plugin>(
            id, luaL_newstate(), metadata(name, permitted),
            QDir(this->app->paths_.pluginsDirectory).absoluteFilePath(id));
        auto *raw = plugin.get();
        PluginControllerAccess::plugins().insert({id, std::move(plugin)});
        PluginControllerAccess::openLibrariesFor(raw);
        this->states.push_back(std::make_unique<sol::state_view>(raw->state()));
        return *this->states.back();
    }

    static sol::protected_function_result run(sol::state_view &lua,
                                              const char *source)
    {
        return lua.safe_script(source, sol::script_pass_on_error);
    }

    std::unique_ptr<MockApplication> app;
    std::vector<std::unique_ptr<sol::state_view>> states;
};

TEST_F(PluginControlTabTest, PermissionIsDedicatedAndRequired)
{
    auto permission = controlTabPermission();
    EXPECT_TRUE(permission.isValid());
    EXPECT_EQ(permission.type, PluginPermission::Type::UiControlTabs);
    EXPECT_TRUE(permission.toHtml().contains(QStringLiteral("control tab")));

    auto &lua = this->addPlugin(QStringLiteral("untrusted"),
                                QStringLiteral("Untrusted"), false);
    auto result = run(lua, R"lua(
        return c2.ui.register_control_tab({ title = "Nope" })
    )lua");
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(QString::fromUtf8(sol::error(result).what())
                    .contains(QStringLiteral("UiControlTabs")));
    EXPECT_TRUE(this->app->plugins.controlTabs().empty());
}

TEST_F(PluginControlTabTest, RegistersBoundedStaticStatusAndUpdatesSummary)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha tools"));
    ASSERT_TRUE(run(lua, R"lua(
        handle = c2.ui.register_control_tab({
            title = "Build monitor",
            icon = "status",
            tooltip = "Local build state",
            accessible_label = "Build monitor status",
            summary = {
                primary_text = "Ready",
                secondary_text = "No build is running",
                value = 3,
                units = "jobs",
                severity = "success",
            },
            controls = {
                {
                    id = "queue",
                    type = "status",
                    label = "Queue",
                    text = "Idle",
                    value = 3,
                    units = "jobs",
                    progress = 0.25,
                },
            },
        })
        assert(handle:is_valid())
    )lua").valid());

    auto snapshots = this->app->plugins.controlTabs();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].pluginId, QStringLiteral("alpha"));
    EXPECT_EQ(snapshots[0].pluginName, QStringLiteral("Alpha tools"));
    EXPECT_EQ(snapshots[0].title, QStringLiteral("Build monitor"));
    EXPECT_EQ(snapshots[0].summary.primaryText, QStringLiteral("Ready"));
    ASSERT_EQ(snapshots[0].controls.size(), 1);
    EXPECT_EQ(snapshots[0].controls[0].progress, 0.25);

    int updates = 0;
    auto connection = this->app->plugins.onControlTabsUpdated.connect([&] {
        ++updates;
    });
    ASSERT_TRUE(run(lua, R"lua(
        handle:update_summary({ primary_text = "Running", severity = "active" })
        handle:update_summary({ primary_text = "Done", severity = "success" })
    )lua").valid());
    EXPECT_EQ(updates, 0);
    snapshots = this->app->plugins.controlTabs();
    EXPECT_EQ(snapshots[0].summary.primaryText, QStringLiteral("Done"));
    EXPECT_TRUE(snapshots[0].summary.secondaryText.isEmpty());
    QCoreApplication::processEvents();
    EXPECT_EQ(updates, 1);
}

TEST_F(PluginControlTabTest, DuplicateAndInvalidUpdatesAreAtomic)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    ASSERT_TRUE(run(lua, R"lua(
        handle = c2.ui.register_control_tab({
            title = "Controls",
            summary = { primary_text = "Original" },
            controls = {
                { id = "enabled", type = "toggle", label = "Enabled", value = false },
            },
            on_control = function() end,
        })
    )lua").valid());
    const auto original = this->app->plugins.controlTabs().at(0);

    auto duplicate = run(lua, R"lua(
        return c2.ui.register_control_tab({ title = "Replacement" })
    )lua");
    EXPECT_FALSE(duplicate.valid());
    EXPECT_EQ(this->app->plugins.controlTabs().at(0), original);

    auto badSummary = run(lua, R"lua(
        handle:update_summary({ primary_text = "Mutated", stylesheet = "*" })
    )lua");
    EXPECT_FALSE(badSummary.valid());
    EXPECT_EQ(this->app->plugins.controlTabs().at(0), original);

    auto badControl = run(lua, R"lua(
        handle:update_controls({
            { id = "enabled", value = true },
            { id = "missing", enabled = false },
        })
    )lua");
    EXPECT_FALSE(badControl.valid());
    EXPECT_EQ(this->app->plugins.controlTabs().at(0), original);
}

TEST_F(PluginControlTabTest, InteractiveControlsPublishAuthoritativeState)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    ASSERT_TRUE(run(lua, R"lua(
        calls = {}
        handle = c2.ui.register_control_tab({
            title = "Generic controls",
            controls = {
                { id = "state", type = "status", label = "State", text = "Ready" },
                { id = "refresh", type = "action", label = "Refresh" },
                { id = "enabled", type = "toggle", label = "Enabled", value = false },
                {
                    id = "mode",
                    type = "choice",
                    label = "Mode",
                    value = "safe",
                    options = {
                        { value = "safe", label = "Safe" },
                        { value = "fast", label = "Fast" },
                    },
                },
            },
            on_control = function(id, value)
                table.insert(calls, { id = id, value = value })
                if id == "enabled" or id == "mode" then
                    handle:update_controls({ { id = id, value = value } })
                end
            end,
        })
    )lua").valid());

    EXPECT_TRUE(this->app->plugins
                    .invokeControlTab(QStringLiteral("alpha"),
                                      QStringLiteral("refresh"),
                                      std::monostate{})
                    .has_value());
    EXPECT_TRUE(this->app->plugins
                    .invokeControlTab(QStringLiteral("alpha"),
                                      QStringLiteral("enabled"), true)
                    .has_value());
    EXPECT_TRUE(this->app->plugins
                    .invokeControlTab(QStringLiteral("alpha"),
                                      QStringLiteral("mode"),
                                      QStringLiteral("fast"))
                    .has_value());
    const auto snapshot = this->app->plugins.controlTabs().at(0);
    EXPECT_TRUE(snapshot.controls[2].toggleValue);
    EXPECT_EQ(snapshot.controls[3].choiceValue, QStringLiteral("fast"));
    EXPECT_EQ(lua["calls"].get<sol::table>().size(), 3);

    ASSERT_TRUE(run(lua, R"lua(
        handle:update_controls({ { id = "enabled", pending = true } })
    )lua").valid());
    auto pending = this->app->plugins.invokeControlTab(
        QStringLiteral("alpha"), QStringLiteral("enabled"), false);
    EXPECT_FALSE(pending.has_value());
    EXPECT_TRUE(pending.error().contains(QStringLiteral("not currently")));
}

TEST_F(PluginControlTabTest, CallbackFailureRollsBackPublishedState)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    ASSERT_TRUE(run(lua, R"lua(
        handle = c2.ui.register_control_tab({
            title = "Rollback",
            summary = { primary_text = "Stable" },
            controls = {
                { id = "break", type = "action", label = "Break" },
            },
            on_control = function()
                handle:update_summary({ primary_text = "Should roll back" })
                error("deliberate callback failure")
            end,
        })
    )lua").valid());

    auto result = this->app->plugins.invokeControlTab(
        QStringLiteral("alpha"), QStringLiteral("break"), std::monostate{});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().contains(QStringLiteral("deliberate")));
    EXPECT_EQ(this->app->plugins.controlTabs().at(0).summary.primaryText,
              QStringLiteral("Stable"));
}

TEST_F(PluginControlTabTest, CallbackReentryIsRejectedWithoutDisablingOwner)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    Expected<void, QString> nested;
    lua.set_function("reenter", [this, &nested] {
        nested = this->app->plugins.invokeControlTab(
            QStringLiteral("alpha"), QStringLiteral("run"),
            std::monostate{});
    });
    ASSERT_TRUE(run(lua, R"lua(
        calls = 0
        handle = c2.ui.register_control_tab({
            title = "Reentry",
            controls = {
                { id = "run", type = "action", label = "Run" },
            },
            on_control = function()
                calls = calls + 1
                reenter()
            end,
        })
    )lua").valid());

    auto outer = this->app->plugins.invokeControlTab(
        QStringLiteral("alpha"), QStringLiteral("run"), std::monostate{});
    EXPECT_TRUE(outer.has_value());
    EXPECT_FALSE(nested.has_value());
    EXPECT_TRUE(nested.error().contains(QStringLiteral("re-entry")));
    EXPECT_EQ(lua["calls"].get<int>(), 1);
    EXPECT_EQ(this->app->plugins.controlTabs().size(), 1);
}

TEST_F(PluginControlTabTest, RemoveInvalidatesHandleImmediately)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    ASSERT_TRUE(run(lua, R"lua(
        handle = c2.ui.register_control_tab({ title = "Temporary" })
        handle:remove()
        assert(not handle:is_valid())
    )lua").valid());
    EXPECT_TRUE(this->app->plugins.controlTabs().empty());

    auto stale = run(lua, R"lua(
        handle:set_visible(true)
    )lua");
    EXPECT_FALSE(stale.valid());
    EXPECT_TRUE(QString::fromUtf8(sol::error(stale).what())
                    .contains(QStringLiteral("stale")));
}

TEST_F(PluginControlTabTest, ReloadFailureAndReplacementAreGenerationScoped)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    ASSERT_TRUE(run(lua, R"lua(
        handle = c2.ui.register_control_tab({ title = "Original" })
    )lua").valid());
    auto original =
        PluginControllerAccess::plugins().at(QStringLiteral("alpha"))
            ->controlTab();
    ASSERT_NE(original, nullptr);

    const auto pluginPath =
        QDir(this->app->paths_.pluginsDirectory).filePath(
            QStringLiteral("alpha"));
    ASSERT_TRUE(QDir().mkpath(pluginPath));
    ASSERT_TRUE(writeFile(
        QDir(pluginPath).filePath(QStringLiteral("info.json")),
        R"json({
            "name": "Reloaded Alpha",
            "description": "Control-tab lifecycle fixture",
            "authors": ["Chatterino tests"],
            "license": "MIT",
            "version": "1.0.0",
            "permissions": [{"type": "UiControlTabs"}]
        })json"));
    ASSERT_TRUE(writeFile(
        QDir(pluginPath).filePath(QStringLiteral("init.lua")),
        R"lua(
            replacement = c2.ui.register_control_tab({ title = "Must vanish" })
            error("deliberate initialization failure")
        )lua"));

    this->app->settings.pluginsEnabled.setValue(true);
    this->app->settings.enabledPlugins.setValue({QStringLiteral("alpha")});
    this->states.clear();
    ASSERT_TRUE(this->app->plugins.reload(QStringLiteral("alpha")));
    EXPECT_FALSE(original->isActive());
    EXPECT_TRUE(this->app->plugins.controlTabs().empty());

    ASSERT_TRUE(writeFile(
        QDir(pluginPath).filePath(QStringLiteral("init.lua")),
        R"lua(
            replacement = c2.ui.register_control_tab({ title = "Replacement" })
        )lua"));
    ASSERT_TRUE(this->app->plugins.reload(QStringLiteral("alpha")));
    const auto snapshots = this->app->plugins.controlTabs();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].pluginName, QStringLiteral("Reloaded Alpha"));
    EXPECT_EQ(snapshots[0].title, QStringLiteral("Replacement"));
    const auto replacement =
        PluginControllerAccess::plugins().at(QStringLiteral("alpha"))
            ->controlTab();
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement, original);
    EXPECT_TRUE(replacement->isActive());
    EXPECT_FALSE(original->isActive());
}

TEST_F(PluginControlTabTest, MultiplePluginsSortAndRespectGlobalLimit)
{
    for (std::size_t index = 0;
         index < PluginController::MAX_CONTROL_TABS; ++index)
    {
        const auto id = QStringLiteral("plugin-%1").arg(
            index, 2, 10, QLatin1Char('0'));
        auto &lua = this->addPlugin(id, QStringLiteral("Plugin %1").arg(index));
        ASSERT_TRUE(run(lua, R"lua(
            handle = c2.ui.register_control_tab({ title = "Status" })
        )lua").valid());
    }
    auto snapshots = this->app->plugins.controlTabs();
    ASSERT_EQ(snapshots.size(), PluginController::MAX_CONTROL_TABS);
    EXPECT_EQ(snapshots.front().pluginId, QStringLiteral("plugin-00"));
    EXPECT_EQ(snapshots.back().pluginId, QStringLiteral("plugin-11"));

    auto &overflow =
        this->addPlugin(QStringLiteral("plugin-over"), QStringLiteral("Over"));
    auto result = run(overflow, R"lua(
        return c2.ui.register_control_tab({ title = "Too many" })
    )lua");
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(this->app->plugins.controlTabs().size(),
              PluginController::MAX_CONTROL_TABS);

    PluginControlTabHost host(nullptr);
    EXPECT_LT(host.minimumSizeHint().width(), 900);
    host.resize(900, 240);
    host.show();
    QCoreApplication::processEvents();
    EXPECT_GE(host.overflowTabCount(), 1);
    EXPECT_LE(host.width(), 900);
}

TEST_F(PluginControlTabTest, HostRendersNativeControlsAndBoundedOverflow)
{
    auto &alpha = this->addPlugin(QStringLiteral("alpha"),
                                  QStringLiteral("Alpha"));
    auto &beta =
        this->addPlugin(QStringLiteral("beta"), QStringLiteral("Beta"));
    ASSERT_TRUE(run(alpha, R"lua(
        invoked = 0
        handle = c2.ui.register_control_tab({
            title = "Actions",
            summary = { primary_text = "Ready" },
            controls = {
                {
                    id = "state",
                    type = "status",
                    label = "State",
                    text = "Ready",
                    progress = 0.5,
                },
                { id = "run", type = "action", label = "Run" },
                { id = "pending", type = "action", label = "Pending", pending = true },
                { id = "enabled", type = "toggle", label = "Enabled", value = false },
                {
                    id = "mode",
                    type = "choice",
                    label = "Mode",
                    value = "safe",
                    options = {
                        { value = "safe", label = "Safe" },
                        { value = "fast", label = "Fast" },
                    },
                },
            },
            on_control = function(id, value)
                invoked = invoked + 1
                if id == "enabled" or id == "mode" then
                    handle:update_controls({ { id = id, value = value } })
                end
            end,
        })
    )lua").valid());
    ASSERT_TRUE(run(beta, R"lua(
        handle = c2.ui.register_control_tab({
            title = "Status",
            summary = { primary_text = "Waiting", open_enabled = false },
        })
    )lua").valid());

    PluginControlTabHost host(nullptr);
    host.resize(900, 240);
    host.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(host.visibleTabCount(), 2);
    EXPECT_EQ(host.overflowTabCount(), 0);
    EXPECT_EQ(host.layoutDirection(), QApplication::layoutDirection());

    auto *alphaButton = host.findChild<QToolButton *>(
        QStringLiteral("plugin-control-tab-alpha"));
    ASSERT_NE(alphaButton, nullptr);
    EXPECT_TRUE(alphaButton->accessibleName().contains(QStringLiteral("Alpha")));
    alphaButton->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(host.openPluginID(), QStringLiteral("alpha"));
    auto *action = host.findChild<QPushButton *>(
        QStringLiteral("plugin-control-alpha-run"));
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(QApplication::focusWidget(), action);
    auto *status = host.findChild<QWidget *>(
        QStringLiteral("plugin-control-alpha-state"));
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->findChild<QProgressBar *>(), nullptr);
    auto *pending = host.findChild<QPushButton *>(
        QStringLiteral("plugin-control-alpha-pending"));
    ASSERT_NE(pending, nullptr);
    EXPECT_FALSE(pending->isEnabled());
    action->click();
    EXPECT_EQ(alpha["invoked"].get<int>(), 1);

    auto *toggle = host.findChild<QCheckBox *>(
        QStringLiteral("plugin-control-alpha-enabled"));
    ASSERT_NE(toggle, nullptr);
    toggle->click();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    toggle = host.findChild<QCheckBox *>(
        QStringLiteral("plugin-control-alpha-enabled"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isChecked());

    auto *choice = host.findChild<QComboBox *>(
        QStringLiteral("plugin-control-alpha-mode"));
    ASSERT_NE(choice, nullptr);
    choice->showPopup();
    QCoreApplication::processEvents();
    auto *popup = QApplication::activePopupWidget();
    ASSERT_NE(popup, nullptr);
    QMouseEvent popupPress(QEvent::MouseButtonPress, QPointF(2, 2),
                           QPointF(2, 2), Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(popup, &popupPress);
    EXPECT_EQ(host.openPluginID(), QStringLiteral("alpha"));
    choice->hidePopup();
    choice->setCurrentIndex(1);
    ASSERT_TRUE(QMetaObject::invokeMethod(choice, "activated",
                                          Qt::DirectConnection,
                                          Q_ARG(int, 1)));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    choice = host.findChild<QComboBox *>(
        QStringLiteral("plugin-control-alpha-mode"));
    ASSERT_NE(choice, nullptr);
    EXPECT_EQ(choice->currentData().toString(), QStringLiteral("fast"));

    QLineEdit focusTarget(&host);
    focusTarget.show();
    focusTarget.setFocus(Qt::OtherFocusReason);
    ASSERT_TRUE(run(alpha, R"lua(
        handle:update_summary({ primary_text = "Still ready" })
    )lua").valid());
    QCoreApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &focusTarget);

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(host.findChild<QWidget *>(
                                QStringLiteral("plugin-control-alpha-mode")),
                            &escape);
    EXPECT_TRUE(host.openPluginID().isEmpty());

    // A top-level widget may otherwise retain the layout-derived minimum
    // width in the headless platform. Constrain it explicitly so this test
    // exercises the same narrow-window overflow path as the UI harness.
    host.setFixedWidth(120);
    host.resize(120, 240);
    QCoreApplication::processEvents();
    EXPECT_GE(host.overflowTabCount(), 1);
    auto *overflowButton = host.findChild<QToolButton *>(
        QStringLiteral("plugin-control-tab-overflow"));
    ASSERT_NE(overflowButton, nullptr);
    EXPECT_TRUE(overflowButton->isVisible());

    host.setOverrideScale(2.0F);
    EXPECT_FLOAT_EQ(host.scale(), 2.0F);
    host.setLayoutDirection(Qt::RightToLeft);
    EXPECT_EQ(host.layoutDirection(), Qt::RightToLeft);
    ASSERT_TRUE(run(alpha, "handle:remove()").valid());
    QCoreApplication::processEvents();
    EXPECT_EQ(host.visibleTabCount(), 1);
}

TEST_F(PluginControlTabTest, HostileDescriptorsFailWithoutRetainedState)
{
    auto &lua = this->addPlugin(QStringLiteral("alpha"),
                                QStringLiteral("Alpha"));
    auto longTitle = QString(49, QLatin1Char('x'));
    lua["long_title"] = longTitle;
    EXPECT_FALSE(run(lua, R"lua(
        return c2.ui.register_control_tab({ title = long_title })
    )lua").valid());
    EXPECT_FALSE(run(lua, R"lua(
        return c2.ui.register_control_tab({
            title = "Unsafe icon",
            icon = "https://example.invalid/icon.png",
        })
    )lua").valid());
    EXPECT_FALSE(run(lua, R"lua(
        return c2.ui.register_control_tab({
            title = "Duplicate IDs",
            controls = {
                { id = "same", type = "status", label = "One" },
                { id = "same", type = "status", label = "Two" },
            },
        })
    )lua").valid());
    EXPECT_FALSE(run(lua, R"lua(
        return c2.ui.register_control_tab({
            title = "Sparse controls",
            controls = {
                [1] = { id = "one", type = "status", label = "One" },
                [3] = { id = "three", type = "status", label = "Three" },
            },
        })
    )lua").valid());
    EXPECT_FALSE(run(lua, R"lua(
        return c2.ui.register_control_tab({
            title = "Hashed choices",
            controls = {
                {
                    id = "choice",
                    type = "choice",
                    label = "Choice",
                    value = "safe",
                    options = {
                        { value = "safe", label = "Safe" },
                        metadata = { value = "ignored", label = "Ignored" },
                    },
                },
            },
            on_control = function() end,
        })
    )lua").valid());
    EXPECT_TRUE(this->app->plugins.controlTabs().empty());
}

#endif
