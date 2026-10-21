#include "capture.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace hyprsession {
namespace {

std::vector<std::string> readArgv(int pid) {
    if (pid <= 0)
        return {};

    std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!in)
        return {};

    const std::string        raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::string> argv;
    std::string              current;
    for (char ch : raw) {
        if (ch == '\0') {
            if (!current.empty())
                argv.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty())
        argv.push_back(std::move(current));
    return argv;
}

std::string readCwd(int pid) {
    if (pid <= 0)
        return {};

    char       buffer[4096];
    const auto path = "/proc/" + std::to_string(pid) + "/cwd";
    const auto length = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    if (length < 0)
        return {};
    buffer[length] = '\0';
    return buffer;
}

std::string monitorName(const PHLMONITORREF& monitor) {
    const auto live = monitor.lock();
    return live ? live->m_name : std::string{};
}

WindowRecord recordFor(PHLWINDOW window, PHLWINDOW focused, std::unordered_map<const void*, std::string>& groupIds, int& nextGroup) {
    WindowRecord record;
    const auto&  meta = window->metadata();
    record.appId        = meta.appID();
    record.initialAppId = meta.initialAppID();
    record.title        = meta.title();
    record.initialTitle = meta.initialTitle();
    record.pid          = static_cast<int>(window->backend().pid());
    record.argv         = readArgv(record.pid);
    record.cwd          = readCwd(record.pid);
    record.floating     = window->isFloating();
    record.pinned       = static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED);
    record.focused      = focused && focused.get() == window.get();
    record.monitor      = monitorName(window->m_monitor);

    if (window->m_workspace) {
        record.workspace = window->m_workspace->addressableName();
    } else {
        record.workspace = window->workspaceAddress();
    }

    if (window->layoutTarget()) {
        const auto box = window->layoutBox();
        if (box.w > 0 && box.h > 0) {
            record.hasBox = true;
            record.box    = Box{.x = box.x, .y = box.y, .w = box.w, .h = box.h};
        }
    }

    if (const auto& controller = Fullscreen::controller())
        record.fullscreen = static_cast<int>(controller->getFullscreenModes(window).internal);

    if (const auto& group = window->grouping().group()) {
        const auto [it, inserted] = groupIds.emplace(group.get(), "");
        if (inserted)
            it->second = "g" + std::to_string(nextGroup++);
        record.groupId      = it->second;
        record.groupLocked  = group->locked();
        record.groupCurrent = group->current().get() == window.get();
        const auto& members = group->windows();
        for (size_t index = 0; index < members.size(); ++index) {
            if (members[index].lock().get() != window.get())
                continue;
            record.groupIndex = static_cast<int>(index);
            break;
        }
    }

    return record;
}

std::string lowerKey(const WindowRecord& record) {
    auto key = classKey(record.appId, record.initialAppId);
    for (char& ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}

std::string readComm(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string   comm;
    if (!std::getline(in, comm))
        return {};
    return comm;
}

std::vector<int> childPids(int pid) {
    std::ifstream    in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children");
    std::vector<int> children;
    int              child = 0;
    while (in >> child)
        children.push_back(child);
    return children;
}

bool isShell(const std::string& comm) {
    return comm == "zsh" || comm == "bash" || comm == "fish" || comm == "sh" || comm == "nu";
}

std::string baseOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isAgent(const std::vector<std::string>& argv) {
    if (argv.empty())
        return false;
    const auto program = baseOf(argv.front());
    if (program == "agent" || program == "cursor-agent")
        return true;
    for (const auto& arg : argv) {
        if (arg.find("cursor-agent") != std::string::npos)
            return true;
    }
    return false;
}

int findShell(int pid, int depth) {
    if (depth > 4)
        return 0;
    for (const int child : childPids(pid)) {
        if (isShell(readComm(child)))
            return child;
        if (const int nested = findShell(child, depth + 1))
            return nested;
    }
    return 0;
}

int findAgent(int pid, int depth) {
    if (depth > 6)
        return 0;
    for (const int child : childPids(pid)) {
        if (isAgent(readArgv(child)))
            return child;
        if (const int nested = findAgent(child, depth + 1))
            return nested;
    }
    return 0;
}

std::string chatFromFds(int pid) {
    std::error_code ec;
    const auto      dir = std::filesystem::path{"/proc"} / std::to_string(pid) / "fd";
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (ec)
            break;
        char       buffer[4096];
        const auto length = ::readlink(entry.path().c_str(), buffer, sizeof(buffer) - 1);
        if (length < 0)
            continue;
        buffer[length] = '\0';
        if (const auto id = cursorChatId(std::string{buffer}))
            return *id;
    }
    return {};
}

std::string chatFromProcess(int pid) {
    if (const auto direct = chatFromFds(pid); !direct.empty())
        return direct;
    for (const int child : childPids(pid)) {
        if (const auto nested = chatFromFds(child); !nested.empty())
            return nested;
    }
    return {};
}

void enrichLaunch(WindowRecord& record) {
    const auto key = lowerKey(record);
    if (key == "cursor") {
        record.argv = {"cursor"};
        return;
    }
    if (key != "foot" || record.pid <= 0)
        return;

    const int shell = findShell(record.pid, 0);
    if (shell <= 0)
        return;

    const auto cwd = readCwd(shell);
    if (!cwd.empty() && cwd != "/")
        record.cwd = cwd;

    std::string              chat;
    std::vector<std::string> command;
    const int                agent = findAgent(shell, 0);
    if (agent > 0) {
        chat = chatFromProcess(agent);
        if (chat.empty() && !record.cwd.empty())
            command = {"agent", "--workspace", record.cwd};
    } else {
        const auto kids = childPids(shell);
        std::vector<int> programs;
        for (const int child : kids) {
            if (!isShell(readComm(child)))
                programs.push_back(child);
        }
        if (programs.size() == 1)
            command = readArgv(programs.front());
    }

    if (const auto argv = relaunchArgv(key, record.cwd, chat, command))
        record.argv = std::move(*argv);
}

Layout::Tiled::CDwindleAlgorithm* dwindleFor(PHLWORKSPACE workspace) {
    if (!workspace)
        return nullptr;
    const auto space = workspace->space();
    if (!space)
        return nullptr;
    const auto algorithm = space->algorithm();
    if (!algorithm)
        return nullptr;
    const auto& tiled = algorithm->tiledAlgo();
    if (!tiled)
        return nullptr;
    const auto name = std::string_view{typeid(*tiled.get()).name()};
    if (name.find("CDwindleAlgorithm") == std::string_view::npos)
        return nullptr;
    return static_cast<Layout::Tiled::CDwindleAlgorithm*>(tiled.get());
}

std::optional<SplitNode> serializeNode(const SP<Layout::Tiled::SDwindleNodeData>& node, const std::unordered_map<const Desktop::View::CWindow*, int>& indexOf) {
    if (!node)
        return std::nullopt;

    SplitNode out;
    if (!node->isNode) {
        const auto target = node->pTarget.lock();
        const auto window = target ? target->window() : nullptr;
        if (!window)
            return std::nullopt;
        const auto found = indexOf.find(window.get());
        if (found == indexOf.end())
            return std::nullopt;
        out.leaf   = true;
        out.window = found->second;
        return out;
    }

    const auto left  = node->children[0].lock();
    const auto right = node->children[1].lock();
    auto       first = serializeNode(left, indexOf);
    auto       second = serializeNode(right, indexOf);
    if (!first || !second)
        return std::nullopt;
    out.splitTop = node->splitTop;
    out.ratio    = node->splitRatio;
    out.children.push_back(std::move(*first));
    out.children.push_back(std::move(*second));
    return out;
}

void captureSplits(Snapshot& snapshot, const std::vector<std::pair<PHLWINDOW, size_t>>& held) {
    std::unordered_map<const Desktop::View::CWindow*, int> indexOf;
    indexOf.reserve(held.size());
    for (const auto& [window, index] : held)
        indexOf.emplace(window.get(), static_cast<int>(index));

    for (const auto& workspace : snapshot.workspaces) {
        PHLWORKSPACE live;
        for (const auto& [window, index] : held) {
            (void)index;
            if (window->m_workspace && window->m_workspace->addressableName() == workspace.address) {
                live = window->m_workspace;
                break;
            }
        }
        auto* dwindle = dwindleFor(live);
        if (!dwindle)
            continue;

        SP<Layout::Tiled::SDwindleNodeData> root;
        bool                                 conflict = false;
        for (const auto& [window, index] : held) {
            (void)index;
            if (!window->m_workspace || window->m_workspace->addressableName() != workspace.address || window->isFloating())
                continue;
            auto node = dwindle->getNodeFromWindow(window);
            if (!node)
                continue;
            while (const auto parent = node->pParent.lock())
                node = parent;
            if (!root)
                root = node;
            else if (root.get() != node.get())
                conflict = true;
        }
        if (!root || conflict)
            continue;

        auto tree = serializeNode(root, indexOf);
        if (!tree)
            continue;
        snapshot.splits.push_back(WorkspaceSplit{.workspace = workspace.address, .root = std::move(*tree)});
    }
}

} // namespace

Snapshot captureLayout() {
    Snapshot snapshot;
    PHLWINDOW focused;
    if (const auto focus = Desktop::focusState())
        focused = focus->window();

    if (const auto& monitors = State::monitorState()) {
        for (const auto& monitor : monitors->monitors()) {
            if (!monitor)
                continue;
            MonitorRecord record;
            record.name        = monitor->m_name;
            record.description = monitor->m_description;
            if (monitor->m_activeWorkspace)
                record.activeWorkspace = monitor->m_activeWorkspace->addressableName();
            if (monitor->m_activeSpecialWorkspace)
                record.activeSpecial = monitor->m_activeSpecialWorkspace->addressableName();
            if (monitor->m_activeWorkspace) {
                if (const auto last = monitor->m_activeWorkspace->getLastFocusedWindow()) {
                    record.focusedAppId = classKey(last->metadata().appID(), last->metadata().initialAppID());
                    record.focusedInitialTitle = last->metadata().initialTitle();
                }
            }
            snapshot.monitors.push_back(std::move(record));
        }
    }

    if (const auto& workspaces = State::workspaceState()) {
        for (const auto& workspace : workspaces->workspaces()) {
            if (!workspace)
                continue;
            WorkspaceRecord record;
            record.address     = workspace->addressableName();
            record.displayName = workspace->displayName();
            record.type        = workspace->type() == Workspace::eWorkspaceType::SPECIAL ? "special" : "normal";
            record.monitor     = monitorName(workspace->m_monitor);
            if (const auto id = workspace->numberedID())
                record.numberedId = static_cast<int>(*id);
            snapshot.workspaces.push_back(std::move(record));
        }
    }

    if (const auto& windows = Desktop::windowState()) {
        std::unordered_map<const void*, std::string> groupIds;
        int                                          nextGroup = 0;
        std::vector<std::pair<PHLWINDOW, size_t>>    held;
        for (const auto& window : windows->windows()) {
            if (!window || !window->mapped())
                continue;
            auto record = recordFor(window, focused, groupIds, nextGroup);
            enrichLaunch(record);
            if (record.appId.empty() && record.initialAppId.empty() && record.argv.empty())
                continue;
            held.emplace_back(window, snapshot.windows.size());
            snapshot.windows.push_back(std::move(record));
        }
        captureSplits(snapshot, held);
    }

    return snapshot;
}

std::vector<OpenWindow> openWindows() {
    std::vector<OpenWindow> open;
    const auto&             windows = Desktop::windowState();
    if (!windows)
        return open;

    for (const auto& window : windows->windows()) {
        if (!window || !window->mapped())
            continue;
        const auto& meta = window->metadata();
        if (meta.appID().empty() && meta.initialAppID().empty())
            continue;
        open.push_back(OpenWindow{
            .identity =
                LiveWindow{
                    .appId        = meta.appID(),
                    .initialAppId = meta.initialAppID(),
                    .initialTitle = meta.initialTitle(),
                },
            .window = window,
        });
    }
    return open;
}

} // namespace hyprsession
