#include "snapshot.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace hyprsession {
namespace {

std::string lowerCopy(std::string text) {
    for (char& ch : text)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

std::string jsonString(const nlohmann::json& item, const char* key) {
    if (!item.contains(key) || !item[key].is_string())
        return {};
    return item[key].get<std::string>();
}

bool jsonBool(const nlohmann::json& item, const char* key) {
    return item.contains(key) && item[key].is_boolean() && item[key].get<bool>();
}

int jsonInt(const nlohmann::json& item, const char* key, int fallback = 0) {
    if (!item.contains(key) || !item[key].is_number_integer())
        return fallback;
    return item[key].get<int>();
}

nlohmann::json windowJson(const WindowRecord& window) {
    nlohmann::json item;
    item["appId"]          = window.appId;
    item["initialAppId"]   = window.initialAppId;
    item["title"]          = window.title;
    item["initialTitle"]   = window.initialTitle;
    item["pid"]            = window.pid;
    item["argv"]           = window.argv;
    item["cwd"]            = window.cwd;
    item["workspace"]      = window.workspace;
    item["monitor"]        = window.monitor;
    item["floating"]       = window.floating;
    item["pinned"]         = window.pinned;
    item["fullscreen"]     = window.fullscreen;
    item["groupId"]        = window.groupId;
    item["groupIndex"]     = window.groupIndex;
    item["groupCurrent"]   = window.groupCurrent;
    item["groupLocked"]    = window.groupLocked;
    item["focused"]        = window.focused;
    if (window.hasBox) {
        item["box"] = {
            {"x", window.box.x},
            {"y", window.box.y},
            {"w", window.box.w},
            {"h", window.box.h},
        };
    } else {
        item["box"] = nullptr;
    }
    return item;
}

std::optional<WindowRecord> windowFromJson(const nlohmann::json& item) {
    if (!item.is_object())
        return std::nullopt;

    WindowRecord window;
    window.appId          = jsonString(item, "appId");
    window.initialAppId   = jsonString(item, "initialAppId");
    window.title          = jsonString(item, "title");
    window.initialTitle   = jsonString(item, "initialTitle");
    window.pid            = jsonInt(item, "pid");
    window.cwd            = jsonString(item, "cwd");
    window.workspace      = jsonString(item, "workspace");
    window.monitor        = jsonString(item, "monitor");
    window.floating       = jsonBool(item, "floating");
    window.pinned         = jsonBool(item, "pinned");
    window.fullscreen     = jsonInt(item, "fullscreen");
    window.groupId        = jsonString(item, "groupId");
    window.groupIndex     = jsonInt(item, "groupIndex");
    window.groupCurrent   = jsonBool(item, "groupCurrent");
    window.groupLocked    = jsonBool(item, "groupLocked");
    window.focused        = jsonBool(item, "focused");

    if (item.contains("argv") && item["argv"].is_array()) {
        for (const auto& arg : item["argv"]) {
            if (arg.is_string())
                window.argv.push_back(arg.get<std::string>());
        }
    }

    if (item.contains("box") && item["box"].is_object()) {
        window.hasBox = true;
        window.box.x  = item["box"].value("x", 0.0);
        window.box.y  = item["box"].value("y", 0.0);
        window.box.w  = item["box"].value("w", 0.0);
        window.box.h  = item["box"].value("h", 0.0);
    }

    return window;
}

bool sameClass(const WindowRecord& saved, const LiveWindow& live) {
    const auto savedKey = classKey(saved.appId, saved.initialAppId);
    const auto liveKey  = classKey(live.appId, live.initialAppId);
    return !savedKey.empty() && savedKey == liveKey;
}

bool sameTitle(const WindowRecord& saved, const LiveWindow& live) {
    return sameClass(saved, live) && !saved.initialTitle.empty() && saved.initialTitle == live.initialTitle;
}

} // namespace

std::string classKey(const std::string& appId, const std::string& initialAppId) {
    if (!initialAppId.empty())
        return initialAppId;
    return appId;
}

std::vector<std::string> splitSkipList(const std::string& text) {
    std::vector<std::string> classes;
    std::string              current;
    for (char ch : text) {
        if (ch == ',') {
            const auto start = current.find_first_not_of(" \t");
            const auto end   = current.find_last_not_of(" \t");
            if (start != std::string::npos)
                classes.push_back(current.substr(start, end - start + 1));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    const auto start = current.find_first_not_of(" \t");
    const auto end   = current.find_last_not_of(" \t");
    if (start != std::string::npos)
        classes.push_back(current.substr(start, end - start + 1));
    return classes;
}

bool skippedClass(const std::string& appId, const std::vector<std::string>& extra) {
    const auto key = lowerCopy(appId);
    if (key.empty())
        return false;
    if (key.starts_with("xdg-desktop-portal"))
        return true;
    if (key == "polkit" || key == "polkit-gnome" || key == "polkit-kde-authentication-agent-1" || key == "hyprland")
        return true;
    for (const auto& name : extra) {
        if (lowerCopy(name) == key)
            return true;
    }
    return false;
}

bool looksLikeFlatpakId(const std::string& appId) {
    return appId.find('.') != std::string::npos && appId.find('/') == std::string::npos && appId.find(' ') == std::string::npos;
}

std::vector<std::string> launchArgv(const WindowRecord& window) {
    if (window.argv.empty())
        return {};

    const auto key = classKey(window.appId, window.initialAppId);
    if (baseName(window.argv.front()) == "bwrap" && looksLikeFlatpakId(key))
        return {"flatpak", "run", key};
    return window.argv;
}

namespace {

bool isUuid(const std::string& id) {
    if (id.size() != 36)
        return false;
    for (size_t index = 0; index < id.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash) {
            if (id[index] != '-')
                return false;
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>(id[index]);
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}

} // namespace

std::optional<std::string> cursorChatId(const std::string& path) {
    const std::string marker = "/.config/cursor/chats/";
    const auto        start  = path.find(marker);
    if (start == std::string::npos)
        return std::nullopt;

    const auto hash = path.find('/', start + marker.size());
    if (hash == std::string::npos)
        return std::nullopt;
    const auto idStart = hash + 1;
    const auto idEnd   = path.find('/', idStart);
    if (idEnd == std::string::npos)
        return std::nullopt;

    const auto id = path.substr(idStart, idEnd - idStart);
    if (path.substr(idEnd) != "/store.db" || !isUuid(id))
        return std::nullopt;
    return id;
}

std::optional<std::vector<std::string>> relaunchArgv(const std::string& appId, const std::string& cwd, const std::string& chatId, const std::vector<std::string>& command) {
    const auto key = lowerCopy(appId);
    if (key == "cursor")
        return std::vector<std::string>{"cursor"};

    if (key != "foot" || cwd.empty() || cwd == "/")
        return std::nullopt;

    std::vector<std::string> argv{"foot", "-D", cwd};
    if (!chatId.empty())
        argv.insert(argv.end(), {"agent", "--resume", chatId, "--workspace", cwd});
    else if (!command.empty())
        argv.insert(argv.end(), command.begin(), command.end());
    return argv;
}

namespace {

nlohmann::json splitJson(const SplitNode& node) {
    nlohmann::json item;
    if (node.leaf) {
        item["leaf"]   = true;
        item["window"] = node.window;
        return item;
    }
    item["leaf"]     = false;
    item["splitTop"] = node.splitTop;
    item["ratio"]    = node.ratio;
    item["children"] = nlohmann::json::array();
    for (const auto& child : node.children)
        item["children"].push_back(splitJson(child));
    return item;
}

std::optional<SplitNode> splitFromJson(const nlohmann::json& item) {
    if (!item.is_object())
        return std::nullopt;
    SplitNode node;
    node.leaf = jsonBool(item, "leaf");
    if (node.leaf) {
        node.window = jsonInt(item, "window", -1);
        return node;
    }
    node.splitTop = jsonBool(item, "splitTop");
    node.ratio    = item.contains("ratio") && item["ratio"].is_number() ? item["ratio"].get<double>() : 1;
    if (!item.contains("children") || !item["children"].is_array())
        return std::nullopt;
    for (const auto& child : item["children"]) {
        auto parsed = splitFromJson(child);
        if (!parsed)
            return std::nullopt;
        node.children.push_back(std::move(*parsed));
    }
    return node;
}

} // namespace

std::string toJson(const Snapshot& snapshot) {
    nlohmann::json root;
    root["version"] = snapshot.version;
    root["monitors"] = nlohmann::json::array();
    for (const auto& monitor : snapshot.monitors) {
        nlohmann::json item;
        item["name"]                 = monitor.name;
        item["description"]          = monitor.description;
        item["activeWorkspace"]      = monitor.activeWorkspace;
        item["activeSpecial"]        = monitor.activeSpecial;
        item["focusedAppId"]         = monitor.focusedAppId;
        item["focusedInitialTitle"]  = monitor.focusedInitialTitle;
        root["monitors"].push_back(std::move(item));
    }

    root["workspaces"] = nlohmann::json::array();
    for (const auto& workspace : snapshot.workspaces) {
        nlohmann::json item;
        item["address"]     = workspace.address;
        item["displayName"] = workspace.displayName;
        item["type"]        = workspace.type;
        item["monitor"]     = workspace.monitor;
        item["numberedId"]  = workspace.numberedId;
        root["workspaces"].push_back(std::move(item));
    }

    root["windows"] = nlohmann::json::array();
    for (const auto& window : snapshot.windows)
        root["windows"].push_back(windowJson(window));

    root["splits"] = nlohmann::json::array();
    for (const auto& split : snapshot.splits) {
        nlohmann::json item;
        item["workspace"] = split.workspace;
        item["root"]      = splitJson(split.root);
        root["splits"].push_back(std::move(item));
    }

    return root.dump(2) + '\n';
}

std::optional<Snapshot> fromJson(const std::string& text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }

    if (!root.is_object() || jsonInt(root, "version") != 1)
        return std::nullopt;
    if (root.contains("monitors") && !root["monitors"].is_array())
        return std::nullopt;
    if (root.contains("workspaces") && !root["workspaces"].is_array())
        return std::nullopt;
    if (root.contains("windows") && !root["windows"].is_array())
        return std::nullopt;

    Snapshot snapshot;
    if (root.contains("monitors")) {
        for (const auto& item : root["monitors"]) {
            if (!item.is_object())
                return std::nullopt;
            MonitorRecord monitor;
            monitor.name                = jsonString(item, "name");
            monitor.description         = jsonString(item, "description");
            monitor.activeWorkspace     = jsonString(item, "activeWorkspace");
            monitor.activeSpecial       = jsonString(item, "activeSpecial");
            monitor.focusedAppId        = jsonString(item, "focusedAppId");
            monitor.focusedInitialTitle = jsonString(item, "focusedInitialTitle");
            snapshot.monitors.push_back(std::move(monitor));
        }
    }

    if (root.contains("workspaces")) {
        for (const auto& item : root["workspaces"]) {
            if (!item.is_object())
                return std::nullopt;
            WorkspaceRecord workspace;
            workspace.address     = jsonString(item, "address");
            workspace.displayName = jsonString(item, "displayName");
            workspace.type        = jsonString(item, "type");
            workspace.monitor     = jsonString(item, "monitor");
            workspace.numberedId  = jsonInt(item, "numberedId");
            snapshot.workspaces.push_back(std::move(workspace));
        }
    }

    if (root.contains("windows")) {
        for (const auto& item : root["windows"]) {
            auto window = windowFromJson(item);
            if (!window)
                return std::nullopt;
            snapshot.windows.push_back(std::move(*window));
        }
    }

    if (root.contains("splits") && root["splits"].is_array()) {
        for (const auto& item : root["splits"]) {
            if (!item.is_object() || !item.contains("root"))
                return std::nullopt;
            auto tree = splitFromJson(item["root"]);
            if (!tree)
                return std::nullopt;
            snapshot.splits.push_back(WorkspaceSplit{.workspace = jsonString(item, "workspace"), .root = std::move(*tree)});
        }
    }

    return snapshot;
}

std::vector<Placement> matchWindows(const std::vector<WindowRecord>& saved, const std::vector<LiveWindow>& live) {
    std::vector<bool>      savedUsed(saved.size(), false);
    std::vector<bool>      liveUsed(live.size(), false);
    std::vector<Placement> placements;

    // Title first, so two windows of the same app with different titles stay put.
    const auto take = [&](auto matches) {
        for (size_t savedIndex = 0; savedIndex < saved.size(); ++savedIndex) {
            if (savedUsed[savedIndex])
                continue;
            for (size_t liveIndex = 0; liveIndex < live.size(); ++liveIndex) {
                if (liveUsed[liveIndex] || !matches(saved[savedIndex], live[liveIndex]))
                    continue;
                savedUsed[savedIndex] = true;
                liveUsed[liveIndex]   = true;
                placements.push_back(Placement{.saved = savedIndex, .live = liveIndex});
                break;
            }
        }
    };

    take(sameTitle);
    take(sameClass);
    return placements;
}

std::vector<SpawnRequest> planLaunches(const Snapshot& snapshot, const std::vector<Placement>& placements, const std::vector<std::string>& skipClass) {
    std::unordered_set<size_t> matched;
    matched.reserve(placements.size());
    for (const auto& placement : placements)
        matched.insert(placement.saved);

    struct Group {
        std::vector<size_t> indexes;
    };

    std::vector<Group>                  groups;
    std::unordered_map<int, size_t>     byPid;
    for (size_t index = 0; index < snapshot.windows.size(); ++index) {
        const int pid = snapshot.windows[index].pid;
        if (pid > 0) {
            const auto found = byPid.find(pid);
            if (found == byPid.end()) {
                byPid.emplace(pid, groups.size());
                groups.push_back(Group{.indexes = {index}});
            } else {
                groups[found->second].indexes.push_back(index);
            }
            continue;
        }
        groups.push_back(Group{.indexes = {index}});
    }

    std::vector<SpawnRequest> launches;
    for (const auto& group : groups) {
        bool                    anyMatched = false;
        std::vector<size_t>     pending;
        const WindowRecord*     representative = nullptr;
        for (size_t index : group.indexes) {
            if (matched.contains(index)) {
                anyMatched = true;
                continue;
            }
            pending.push_back(index);
            if (!representative || representative->argv.empty())
                representative = &snapshot.windows[index];
        }
        if (anyMatched || pending.empty() || !representative)
            continue;

        const auto key = classKey(representative->appId, representative->initialAppId);
        if (skippedClass(key, skipClass))
            continue;

        auto argv = launchArgv(*representative);
        if (argv.empty())
            continue;

        SpawnRequest request;
        request.argv    = std::move(argv);
        request.cwd     = representative->cwd;
        request.appId   = key;
        request.pending = std::move(pending);
        launches.push_back(std::move(request));
    }
    return launches;
}

std::optional<size_t> claimPending(const std::vector<WindowRecord>& windows, const std::vector<size_t>& pending, const LiveWindow& live) {
    const auto liveKey = classKey(live.appId, live.initialAppId);
    if (liveKey.empty())
        return std::nullopt;

    std::optional<size_t> fallback;
    for (size_t index : pending) {
        if (index >= windows.size())
            continue;
        const auto& saved = windows[index];
        if (classKey(saved.appId, saved.initialAppId) != liveKey)
            continue;
        if (!live.initialTitle.empty() && live.initialTitle == saved.initialTitle)
            return index;
        if (!fallback)
            fallback = index;
    }
    return fallback;
}

const WorkspaceRecord* findWorkspace(const Snapshot& snapshot, const std::string& address) {
    for (const auto& workspace : snapshot.workspaces) {
        if (workspace.address == address)
            return &workspace;
    }
    return nullptr;
}

} // namespace hyprsession
