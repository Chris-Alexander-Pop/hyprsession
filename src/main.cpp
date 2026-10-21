#include "session.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/version.h>

#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

// The plugin build checks that this Hyprland commit is present in the DSO.
__attribute__((used)) static const char kHyprlandCommit[] = GIT_COMMIT_HASH;

inline HANDLE g_pluginHandle = nullptr;
inline SP<IPC::Socket1::SCommand> g_command;

namespace {

bool addConfigValue(SP<Config::Values::IValue> value) {
    const std::string name = value->name();
    if (HyprlandAPI::addConfigValueV2(g_pluginHandle, value))
        return true;

    HyprlandAPI::addNotification(g_pluginHandle, "[hyprsession] failed to register config value " + name, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    return false;
}

std::string commandArgs(const IPC::Socket1::SRequest& request) {
    std::string_view args = request.command;
    constexpr std::string_view name = "hyprsession";
    if (args.starts_with(name))
        args.remove_prefix(name.size());
    while (!args.empty() && args.front() == ' ')
        args.remove_prefix(1);
    return std::string{args};
}

IPC::Socket1::SResponse hyprctl(const IPC::Socket1::SRequest& request) {
    if (!hyprsession::g_session)
        return "error: hyprsession is not running\n";

    const auto args = commandArgs(request);
    if (args == "restore") {
        const auto result = hyprsession::g_session->restore();
        if (!result.success)
            return "error: " + (result.error.empty() ? std::string{"restore failed"} : result.error) + "\n";
        return "ok\n";
    }
    if (args == "snapshot") {
        const auto result = hyprsession::g_session->snapshotNow();
        if (!result.success)
            return "error: " + (result.error.empty() ? std::string{"snapshot failed"} : result.error) + "\n";
        return "ok\n";
    }
    return "usage: hyprsession restore|snapshot\n";
}

int luaRestore(lua_State* L) {
    if (!hyprsession::g_session) {
        lua_pushstring(L, "hyprsession is not running");
        return lua_error(L);
    }
    const auto result = hyprsession::g_session->restore();
    if (!result.success) {
        lua_pushstring(L, result.error.empty() ? "hyprsession restore failed" : result.error.c_str());
        return lua_error(L);
    }
    return 0;
}

int luaSnapshot(lua_State* L) {
    if (!hyprsession::g_session) {
        lua_pushstring(L, "hyprsession is not running");
        return lua_error(L);
    }
    const auto result = hyprsession::g_session->snapshotNow();
    if (!result.success) {
        lua_pushstring(L, result.error.empty() ? "hyprsession snapshot failed" : result.error.c_str());
        return lua_error(L);
    }
    return 0;
}

SDispatchResult dispatchRestore(const std::string&) {
    if (!hyprsession::g_session)
        return {.success = false, .error = "hyprsession is not running"};
    return hyprsession::g_session->restore();
}

SDispatchResult dispatchSnapshot(const std::string&) {
    if (!hyprsession::g_session)
        return {.success = false, .error = "hyprsession is not running"};
    return hyprsession::g_session->snapshotNow();
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprsession:interval_sec", "", Config::INTEGER{60}));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprsession:restore_timeout_sec", "", Config::INTEGER{20}));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprsession:skip_class", "", Config::STRING{""}));
    addConfigValue(makeShared<Config::Values::CBoolValue>("plugin:hyprsession:restore_on_start", "", false));

    hyprsession::g_session = new hyprsession::Session();
    hyprsession::g_session->start(handle);

    g_command = HyprlandAPI::registerHyprCtlCommand(handle, IPC::Socket1::SCommand{
        .name = "hyprsession",
        .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
        .handler = hyprctl,
    });
    if (!g_command)
        HyprlandAPI::addNotification(handle, "[hyprsession] failed to register hyprctl command", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);

    const bool lua = Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA;
    if (!lua) {
        if (!HyprlandAPI::addDispatcherV2(handle, "hyprsession:restore", dispatchRestore))
            HyprlandAPI::addNotification(handle, "[hyprsession] failed to register dispatcher hyprsession:restore", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        if (!HyprlandAPI::addDispatcherV2(handle, "hyprsession:snapshot", dispatchSnapshot))
            HyprlandAPI::addNotification(handle, "[hyprsession] failed to register dispatcher hyprsession:snapshot", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    } else {
        // addDispatcherV2 fails in Lua mode and would show a startup error. The socket
        // command and hl.plugin.hyprsession.restore are the entry points on this config.
        if (!HyprlandAPI::addLuaFunction(handle, "hyprsession", "restore", luaRestore))
            HyprlandAPI::addNotification(handle, "[hyprsession] failed to register hl.plugin.hyprsession.restore", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        if (!HyprlandAPI::addLuaFunction(handle, "hyprsession", "snapshot", luaSnapshot))
            HyprlandAPI::addNotification(handle, "[hyprsession] failed to register hl.plugin.hyprsession.snapshot", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    }

    HyprlandAPI::reloadConfig();

    return {
        .name = "hyprsession",
        .description = "Periodic layout snapshots and restore",
        .author = "chris",
        .version = "0.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (hyprsession::g_session) {
        hyprsession::g_session->shutdown();
        delete hyprsession::g_session;
        hyprsession::g_session = nullptr;
    }
    if (g_command) {
        HyprlandAPI::unregisterHyprCtlCommand(g_pluginHandle, g_command);
        g_command.reset();
    }
}
