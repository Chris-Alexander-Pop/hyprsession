#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hyprsession {

struct Box {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

struct WindowRecord {
    std::string              appId;
    std::string              initialAppId;
    std::string              title;
    std::string              initialTitle;
    int                      pid = 0;
    std::vector<std::string> argv;
    std::string              cwd;
    std::string              workspace;
    std::string              monitor;
    bool                     floating     = false;
    bool                     pinned       = false;
    int                      fullscreen   = 0;
    bool                     hasBox       = false;
    Box                      box{};
    std::string              groupId;
    int                      groupIndex   = 0;
    bool                     groupCurrent = false;
    bool                     groupLocked  = false;
    bool                     focused      = false;
};

struct WorkspaceRecord {
    std::string address;
    std::string displayName;
    std::string type;
    std::string monitor;
    int         numberedId = 0;
};

struct MonitorRecord {
    std::string name;
    std::string description;
    std::string activeWorkspace;
    std::string activeSpecial;
    std::string focusedAppId;
    std::string focusedInitialTitle;
};

struct SplitNode {
    bool                   leaf     = false;
    int                    window   = -1;
    bool                   splitTop = false;
    double                 ratio    = 1;
    std::vector<SplitNode> children;
};

struct WorkspaceSplit {
    std::string workspace;
    SplitNode   root;
};

struct Snapshot {
    int                           version = 1;
    std::vector<MonitorRecord>    monitors;
    std::vector<WorkspaceRecord>  workspaces;
    std::vector<WindowRecord>     windows;
    std::vector<WorkspaceSplit>   splits;
};

struct LiveWindow {
    std::string appId;
    std::string initialAppId;
    std::string initialTitle;
};

struct Placement {
    size_t saved = 0;
    size_t live  = 0;
};

struct SpawnRequest {
    std::vector<std::string> argv;
    std::string              cwd;
    std::string              appId;
    std::vector<size_t>      pending;
};

std::string classKey(const std::string& appId, const std::string& initialAppId);

std::vector<std::string> splitSkipList(const std::string& text);

bool skippedClass(const std::string& appId, const std::vector<std::string>& extra);

bool looksLikeFlatpakId(const std::string& appId);

std::vector<std::string> launchArgv(const WindowRecord& window);

std::optional<std::string> cursorChatId(const std::string& path);

// nullopt keeps the command captured from /proc. Cursor and foot get a relaunch line.
// chatId resumes that agent chat. command is a foreground program, used when there is no chat.
std::optional<std::vector<std::string>> relaunchArgv(const std::string& appId, const std::string& cwd, const std::string& chatId, const std::vector<std::string>& command = {});

std::string toJson(const Snapshot& snapshot);

std::optional<Snapshot> fromJson(const std::string& text);

std::vector<Placement> matchWindows(const std::vector<WindowRecord>& saved, const std::vector<LiveWindow>& live);

std::vector<SpawnRequest> planLaunches(const Snapshot& snapshot, const std::vector<Placement>& placements, const std::vector<std::string>& skipClass);

std::optional<size_t> claimPending(const std::vector<WindowRecord>& windows, const std::vector<size_t>& pending, const LiveWindow& live);

const WorkspaceRecord* findWorkspace(const Snapshot& snapshot, const std::string& address);

} // namespace hyprsession
