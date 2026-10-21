#pragma once

#include "snapshot.hpp"

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/types.h>

namespace hyprsession {

class Session {
  public:
    void            start(void* handle);
    void            shutdown();
    SDispatchResult snapshotNow();
    SDispatchResult restore();

    void onLayoutChanged();
    void onWindowOpened(PHLWINDOW window);
    void onPeriodic(SP<CEventLoopTimer> timer);
    void onDebounce(SP<CEventLoopTimer> timer);
    void finishRestore();

  private:
    bool flush();
    void place(PHLWINDOW window, size_t index, bool focusNow);
    void present(size_t index);
    bool groupComplete(const std::string& groupId) const;
    void assemble(const std::string& groupId);
    void finishGroups();
    void focusSaved();
    void armRestoreTimer();
    void reapChildren();
    void notify(const std::string& text) const;
    bool shouldOffer() const;
    void spawnOffer();
    void onOfferTick(SP<CEventLoopTimer> timer);
    void finishOffer();

    void*                                  m_handle = nullptr;
    bool                                   m_stopped = false;
    bool                                   m_restoreActive = false;
    bool                                   m_suspendWrites = false;
    std::string                            m_lastWritten;
    Snapshot                               m_snapshot;
    std::unordered_map<size_t, PHLWINDOWREF> m_placed;
    std::vector<size_t>                    m_pending;
    std::unordered_set<size_t>             m_presented;
    int                                    m_launched = 0;
    std::vector<pid_t>                     m_children;
    std::vector<CHyprSignalListener>       m_listeners;
    SP<CEventLoopTimer>                    m_periodic;
    SP<CEventLoopTimer>                    m_debounce;
    SP<CEventLoopTimer>                    m_restoreTimer;
    SP<CEventLoopTimer>                    m_offerTimer;
    pid_t                                  m_offerPid = 0;
    int                                    m_offerFd = -1;
    std::string                            m_offerOutput;
    std::chrono::steady_clock::time_point  m_offerStarted{};
};

extern Session* g_session;

} // namespace hyprsession
