#include "restore.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenTypes.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <typeinfo>
#include <unistd.h>
#include <unordered_set>
#include <vector>

extern char** environ;

namespace hyprsession {
namespace {

void logAction(const char* what, const Config::Actions::ActionResult& result) {
    if (result || !Log::logger)
        return;
    Log::logger->log(Log::ERR, "hyprsession", "{}: {}", what, result.error().message);
}

PHLWORKSPACE workspaceByAddress(const std::string& address) {
    const auto& workspaces = State::workspaceState();
    if (!workspaces || address.empty())
        return nullptr;

    for (const auto& workspace : workspaces->workspaces()) {
        const auto live = workspace.lock();
        if (live && live->addressableName() == address)
            return live;
    }
    return nullptr;
}

bool allDigits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

PHLWORKSPACE ensureWorkspace(const std::string& address, PHLMONITOR monitor, const Snapshot& snapshot) {
    if (auto existing = workspaceByAddress(address))
        return existing;

    const auto& workspaces = State::workspaceState();
    if (!workspaces || !monitor || address.empty())
        return nullptr;

    if (address.starts_with("special:"))
        return workspaces->createSpecial(address, monitor, true);

    const WorkspaceRecord* saved = findWorkspace(snapshot, address);
    const std::string      displayName = saved ? saved->displayName : std::string{};

    if (allDigits(address)) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(address.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && id <= UINT32_MAX)
            return workspaces->createNumbered(Workspace::SWorkspaceNumberedID{static_cast<Workspace::WorkspaceIDContainer>(id)}, monitor, displayName, true);
    }

    if (const auto& resolver = State::Workspace::resolver()) {
        const auto target = resolver->getWorkspaceTargetFromString(address, monitor);
        if (target.valid())
            return workspaces->create(target, monitor, true);
    }

    return workspaces->createNamed(address, monitor, displayName, true);
}

PHLMONITOR monitorFor(const MonitorRecord& record) {
    const auto& monitors = State::monitorState();
    if (!monitors)
        return nullptr;

    PHLMONITOR byDescription;
    for (const auto& monitor : monitors->monitors()) {
        if (!monitor)
            continue;
        if (!record.name.empty() && monitor->m_name == record.name)
            return monitor;
        if (!record.description.empty() && monitor->m_description == record.description)
            byDescription = monitor;
    }
    return byDescription;
}

void showSpecial(PHLMONITOR monitor, const std::string& address, const Snapshot& snapshot) {
    if (!monitor)
        return;

    auto current = monitor->m_activeSpecialWorkspace;
    if (address.empty()) {
        if (current)
            logAction("hide special workspace", Config::Actions::toggleSpecial(current));
        return;
    }
    if (current && current->addressableName() == address)
        return;
    if (current)
        logAction("hide special workspace", Config::Actions::toggleSpecial(current));

    auto special = ensureWorkspace(address, monitor, snapshot);
    if (special)
        logAction("show special workspace", Config::Actions::toggleSpecial(special));
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

struct SplitShape {
    int                     leaves    = 0;
    int                     internals = 0;
    bool                    ok        = true;
    std::unordered_set<int> windows;
};

void measureSplit(const SplitNode& node, SplitShape& shape) {
    if (!shape.ok)
        return;
    if (node.leaf) {
        if (node.window < 0 || !shape.windows.insert(node.window).second)
            shape.ok = false;
        ++shape.leaves;
        return;
    }
    if (node.children.size() != 2) {
        shape.ok = false;
        return;
    }
    ++shape.internals;
    measureSplit(node.children[0], shape);
    measureSplit(node.children[1], shape);
}

void collectNodes(const SP<Layout::Tiled::SDwindleNodeData>& node, std::vector<SP<Layout::Tiled::SDwindleNodeData>>& internals, std::unordered_set<Layout::Tiled::SDwindleNodeData*>& leaves, std::unordered_set<Layout::Tiled::SDwindleNodeData*>& seen) {
    if (!node || !seen.insert(node.get()).second)
        return;
    if (node->isNode)
        internals.push_back(node);
    else
        leaves.insert(node.get());
    collectNodes(node->children[0].lock(), internals, leaves, seen);
    collectNodes(node->children[1].lock(), internals, leaves, seen);
}

struct SplitAssignment {
    SP<Layout::Tiled::SDwindleNodeData> node;
    SP<Layout::Tiled::SDwindleNodeData> parent;
    SP<Layout::Tiled::SDwindleNodeData> left;
    SP<Layout::Tiled::SDwindleNodeData> right;
    bool                                leaf     = false;
    bool                                splitTop = false;
    float                               ratio    = 1.F;
};

void attachParent(const SP<Layout::Tiled::SDwindleNodeData>& node, const SP<Layout::Tiled::SDwindleNodeData>& parent) {
    if (parent)
        node->pParent = parent;
    else
        node->pParent.reset();
}

SP<Layout::Tiled::SDwindleNodeData> planSplit(const SplitNode& saved, const SP<Layout::Tiled::SDwindleNodeData>& parent, std::vector<SP<Layout::Tiled::SDwindleNodeData>>& internals, size_t& cursor, Layout::Tiled::CDwindleAlgorithm* dwindle, const std::function<PHLWINDOW(size_t)>& windowAt, std::vector<SplitAssignment>& plan) {
    if (saved.leaf) {
        const auto window = windowAt(static_cast<size_t>(saved.window));
        const auto node   = window ? dwindle->getNodeFromWindow(window) : nullptr;
        if (!node)
            return nullptr;
        plan.push_back(SplitAssignment{.node = node, .parent = parent, .leaf = true});
        return node;
    }
    if (cursor >= internals.size() || saved.children.size() != 2)
        return nullptr;
    const auto node = internals[cursor++];
    const auto left  = planSplit(saved.children[0], node, internals, cursor, dwindle, windowAt, plan);
    const auto right = planSplit(saved.children[1], node, internals, cursor, dwindle, windowAt, plan);
    if (!left || !right)
        return nullptr;
    plan.push_back(SplitAssignment{
        .node     = node,
        .parent   = parent,
        .left     = left,
        .right    = right,
        .splitTop = saved.splitTop,
        .ratio    = std::clamp(static_cast<float>(saved.ratio), 0.1F, 1.9F),
    });
    return node;
}

void commitSplit(const std::vector<SplitAssignment>& plan) {
    for (const auto& item : plan) {
        if (!item.node)
            continue;
        attachParent(item.node, item.parent);
        if (item.leaf) {
            item.node->children[0].reset();
            item.node->children[1].reset();
            item.node->isNode = false;
            continue;
        }
        item.node->isNode = true;
        item.node->pTarget.reset();
        item.node->splitTop   = item.splitTop;
        item.node->splitRatio = item.ratio;
        item.node->children[0] = item.left;
        item.node->children[1] = item.right;
    }
}

} // namespace

bool commandRunnable(const std::string& argv0) {
    if (argv0.empty())
        return false;
    if (argv0.find('/') != std::string::npos)
        return ::access(argv0.c_str(), X_OK) == 0;

    const char* path = std::getenv("PATH");
    if (!path)
        return false;

    std::string directory;
    for (const char* cursor = path;; ++cursor) {
        if (*cursor != ':' && *cursor != '\0') {
            directory.push_back(*cursor);
            continue;
        }
        if (!directory.empty()) {
            const auto candidate = directory + "/" + argv0;
            if (::access(candidate.c_str(), X_OK) == 0)
                return true;
            directory.clear();
        }
        if (*cursor == '\0')
            break;
    }
    return false;
}

bool moveToSavedWorkspace(PHLWINDOW window, const WindowRecord& record, const Snapshot& snapshot) {
    if (!window || record.workspace.empty())
        return false;

    const auto moved = Config::Actions::moveToWorkspace(record.workspace, true, window);
    if (moved)
        return true;
    logAction("move window", moved);

    PHLMONITOR monitor = window->m_monitor.lock();
    if (!monitor && State::monitorState() && !State::monitorState()->monitors().empty())
        monitor = State::monitorState()->monitors().front();

    auto workspace = ensureWorkspace(record.workspace, monitor, snapshot);
    if (!workspace)
        return false;
    window->moveToWorkspace(workspace);
    return true;
}

void applyChrome(PHLWINDOW window, const WindowRecord& record) {
    if (!window)
        return;

    if (record.floating) {
        if (!window->isFloating())
            logAction("float window", Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE, window));
        if (record.hasBox) {
            logAction("move window", Config::Actions::move(Vector2D{record.box.x, record.box.y}, false, window));
            logAction("resize window", Config::Actions::resize(Vector2D{record.box.w, record.box.h}, false, window));
        }
    } else if (window->isFloating()) {
        logAction("tile window", Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_DISABLE, window));
    }

    const bool pinned = static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED);
    if (record.pinned && !pinned)
        logAction("pin window", Config::Actions::pinWindow(Config::Actions::TOGGLE_ACTION_ENABLE, window));
    else if (!record.pinned && pinned)
        logAction("unpin window", Config::Actions::pinWindow(Config::Actions::TOGGLE_ACTION_DISABLE, window));
}

void applyFullscreen(PHLWINDOW window, const WindowRecord& record) {
    if (!window)
        return;
    if (const auto& controller = Fullscreen::controller()) {
        const auto desired = record.fullscreen == 1 || record.fullscreen == 2 ? static_cast<Fullscreen::eFullscreenMode>(record.fullscreen) : Fullscreen::FSMODE_NONE;
        if (controller->getFullscreenModes(window).internal != desired)
            controller->setFullscreenMode(window, desired, desired, true);
    }
}

int applySplits(const Snapshot& snapshot, const std::function<PHLWINDOW(size_t)>& windowAt) {
    int rebuilt = 0;
    for (const auto& split : snapshot.splits) {
        SplitShape shape;
        measureSplit(split.root, shape);
        if (!shape.ok || shape.leaves == 0)
            continue;

        const auto workspace = workspaceByAddress(split.workspace);
        auto*      dwindle   = dwindleFor(workspace);
        const auto space     = workspace ? workspace->space() : nullptr;
        if (!dwindle || !space)
            continue;

        SP<Layout::Tiled::SDwindleNodeData> root;
        bool                                 sameRoot = true;
        for (const int index : shape.windows) {
            const auto window = windowAt(static_cast<size_t>(index));
            const auto node   = window && !window->isFloating() ? dwindle->getNodeFromWindow(window) : nullptr;
            if (!node || node->isNode) {
                sameRoot = false;
                break;
            }
            auto top = node;
            while (const auto parent = top->pParent.lock())
                top = parent;
            if (!root)
                root = top;
            else if (root.get() != top.get())
                sameRoot = false;
        }
        if (!root || !sameRoot)
            continue;

        std::vector<SP<Layout::Tiled::SDwindleNodeData>> internals;
        std::unordered_set<Layout::Tiled::SDwindleNodeData*> leaves;
        std::unordered_set<Layout::Tiled::SDwindleNodeData*> seen;
        collectNodes(root, internals, leaves, seen);
        if (static_cast<int>(leaves.size()) != shape.leaves || static_cast<int>(internals.size()) != shape.internals)
            continue;

        bool covered = true;
        for (const int index : shape.windows) {
            const auto window = windowAt(static_cast<size_t>(index));
            const auto node   = window ? dwindle->getNodeFromWindow(window) : nullptr;
            if (!node || !leaves.contains(node.get())) {
                covered = false;
                break;
            }
        }
        if (!covered || static_cast<int>(leaves.size()) != shape.leaves)
            continue;

        // New windows split whatever is under the mouse, so replay would not rebuild this tree.
        // The nodes already exist; only the parent links and ratios need to match the snapshot.
        std::vector<SplitAssignment> plan;
        size_t                       cursor = 0;
        const auto                   wired  = planSplit(split.root, nullptr, internals, cursor, dwindle, windowAt, plan);
        if (!wired || cursor != internals.size())
            continue;
        commitSplit(plan);
        wired->box = space->workArea();
        wired->recalcSizePosRecursive(true);
        ++rebuilt;
    }
    return rebuilt;
}

bool assembleGroup(const std::vector<GroupMember>& members) {
    std::vector<PHLWINDOW> windows;
    windows.reserve(members.size());
    for (const auto& member : members) {
        if (member.window)
            windows.push_back(member.window);
    }
    if (windows.size() < 2)
        return false;

    const auto existing = windows.front()->grouping().group();
    bool       shared   = static_cast<bool>(existing) && existing->size() == windows.size();
    if (shared) {
        for (const auto& window : windows) {
            if (!existing->has(window))
                shared = false;
        }
    }

    SP<Desktop::View::CGroup> group = shared ? existing : nullptr;
    if (!group) {
        for (const auto& window : windows) {
            if (const auto current = window->grouping().group(); current && current->has(window))
                current->remove(window);
        }
        std::vector<PHLWINDOWREF> refs;
        refs.reserve(windows.size());
        for (const auto& window : windows)
            refs.emplace_back(window);
        group = Desktop::View::CGroup::create(std::move(refs));
    }
    if (!group)
        return false;

    bool locked = false;
    for (const auto& member : members) {
        if (!member.window)
            continue;
        if (member.current)
            group->setCurrent(member.window);
        locked = locked || member.locked;
    }
    group->setLocked(locked);
    return true;
}

bool applyMonitor(const MonitorRecord& record, const Snapshot& snapshot) {
    auto monitor = monitorFor(record);
    if (!monitor)
        return false;

    logAction("focus monitor", Config::Actions::focusMonitor(monitor));
    if (!record.activeWorkspace.empty()) {
        const auto changed = Config::Actions::changeWorkspace(record.activeWorkspace);
        logAction("change workspace", changed);
        if (!workspaceByAddress(record.activeWorkspace))
            ensureWorkspace(record.activeWorkspace, monitor, snapshot);
        if (auto workspace = workspaceByAddress(record.activeWorkspace)) {
            const auto owner = workspace->m_monitor.lock();
            if (owner.get() != monitor.get())
                logAction("move workspace to monitor", Config::Actions::moveToMonitor(workspace, monitor));
            logAction("change workspace", Config::Actions::changeWorkspace(workspace));
        }
    }

    showSpecial(monitor, record.activeSpecial, snapshot);
    return true;
}

void focusWindow(PHLWINDOW window) {
    if (window)
        logAction("focus window", Config::Actions::focus(window));
}

Spawned spawnRequest(const SpawnRequest& request) {
    Spawned spawned;
    if (request.argv.empty() || !commandRunnable(request.argv.front())) {
        spawned.error = "command is not runnable";
        return spawned;
    }

    std::vector<char*> args;
    args.reserve(request.argv.size() + 1);
    for (const auto& arg : request.argv)
        args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t           attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);

    const bool haveCwd = !request.cwd.empty() && ::access(request.cwd.c_str(), X_OK) == 0;
    if (haveCwd)
        posix_spawn_file_actions_addchdir_np(&actions, request.cwd.c_str());

    const int rc = ::posix_spawnp(&spawned.pid, request.argv.front().c_str(), &actions, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        spawned.error = std::strerror(rc);
        spawned.pid   = 0;
        return spawned;
    }

    spawned.ok = true;
    return spawned;
}

} // namespace hyprsession
