#pragma once

#include "snapshot.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <vector>

namespace hyprsession {

struct OpenWindow {
    LiveWindow identity;
    PHLWINDOW  window;
};

Snapshot captureLayout();

std::vector<OpenWindow> openWindows();

} // namespace hyprsession
