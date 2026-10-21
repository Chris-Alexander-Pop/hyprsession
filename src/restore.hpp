#pragma once

#include "snapshot.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace hyprsession {

struct GroupMember {
    int       index   = 0;
    bool      current = false;
    bool      locked  = false;
    PHLWINDOW window;
};

struct Spawned {
    bool        ok = false;
    pid_t       pid = 0;
    std::string error;
};

bool commandRunnable(const std::string& argv0);

bool moveToSavedWorkspace(PHLWINDOW window, const WindowRecord& record, const Snapshot& snapshot);

void applyChrome(PHLWINDOW window, const WindowRecord& record);

void applyFullscreen(PHLWINDOW window, const WindowRecord& record);

// Returns how many workspace trees were rebuilt. A workspace is skipped when its live tiled windows don't match the snapshot.
int applySplits(const Snapshot& snapshot, const std::function<PHLWINDOW(size_t)>& windowAt);

bool assembleGroup(const std::vector<GroupMember>& members);

bool applyMonitor(const MonitorRecord& record, const Snapshot& snapshot);

void focusWindow(PHLWINDOW window);

Spawned spawnRequest(const SpawnRequest& request);

} // namespace hyprsession
