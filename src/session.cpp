#include "session.hpp"

#include "capture.hpp"
#include "restore.hpp"
#include "store.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

namespace hyprsession {
namespace {

long configInt(const char* name, long fallback) {
    if (!Config::mgr())
        return fallback;

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::INTEGER))
        return static_cast<long>(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr) ? 1L : 0L;
    return fallback;
}

bool configBool(const char* name, bool fallback) {
    if (!Config::mgr())
        return fallback;

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::BOOL) || *value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr);
    if (*value.type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr) != 0;
    return fallback;
}

std::string configString(const char* name) {
    if (!Config::mgr())
        return {};

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type || *value.type != typeid(Config::STRING))
        return {};
    return **reinterpret_cast<Config::STRING* const*>(value.dataptr);
}

int intervalSec() {
    // A zero interval would spin the event loop.
    long value = configInt("plugin:hyprsession:interval_sec", 60);
    if (value < 5)
        value = 5;
    if (value > 3600)
        value = 3600;
    return static_cast<int>(value);
}

int restoreTimeoutSec() {
    long value = configInt("plugin:hyprsession:restore_timeout_sec", 20);
    if (value < 1)
        value = 1;
    if (value > 120)
        value = 120;
    return static_cast<int>(value);
}

int compositorAgeSec() {
    std::ifstream statFile("/proc/self/stat");
    std::string   line;
    if (!std::getline(statFile, line))
        return 100000;
    const auto closeParen = line.rfind(')');
    if (closeParen == std::string::npos)
        return 100000;

    std::istringstream rest(line.substr(closeParen + 2));
    std::string        token;
    for (int field = 0; field < 19; ++field) {
        if (!(rest >> token))
            return 100000;
    }
    unsigned long long startTicks = 0;
    if (!(rest >> startTicks))
        return 100000;

    std::ifstream uptimeFile("/proc/uptime");
    double        uptime = 0;
    if (!(uptimeFile >> uptime))
        return 100000;
    const long ticks = ::sysconf(_SC_CLK_TCK);
    if (ticks <= 0)
        return 100000;
    return static_cast<int>(uptime - static_cast<double>(startTicks) / static_cast<double>(ticks));
}

std::string offerStampPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] != '\0')
        return std::string{runtime} + "/hyprsession-offered";
    return "/run/user/" + std::to_string(::getuid()) + "/hyprsession-offered";
}

bool alreadyOffered() {
    std::ifstream in(offerStampPath());
    int           pid = 0;
    return (in >> pid) && pid == ::getpid();
}

void markOffered() {
    std::ofstream out(offerStampPath(), std::ios::trunc);
    if (out)
        out << ::getpid() << '\n';
}

std::string trimmedAction(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

Session* g_session = nullptr;

void Session::start(void* handle) {
    m_handle = handle;
    std::string error;
    if (auto existing = readSnapshotFile(defaultSnapshotPath(), error))
        m_lastWritten = std::move(*existing);
    else if (!error.empty() && Log::logger)
        Log::logger->log(Log::ERR, "hyprsession", "read snapshot: {}", error);

    if (const auto& bus = Event::bus()) {
        auto& events = bus->m_events;
        m_listeners.emplace_back(events.exit.listen([this] {
            if (!m_restoreActive && !m_suspendWrites)
                flush();
        }));
        m_listeners.emplace_back(events.window.open.listen([this](PHLWINDOW window) {
            if (m_restoreActive)
                onWindowOpened(window);
            else
                onLayoutChanged();
        }));
        m_listeners.emplace_back(events.window.close.listen([this](PHLWINDOW) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.window.floating.listen([this](PHLWINDOW) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.window.fullscreen.listen([this](PHLWINDOW) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.window.pin.listen([this](PHLWINDOW) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.window.title.listen([this](PHLWINDOW) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.workspace.active.listen([this](PHLWORKSPACE) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.workspace.specialActive.listen([this](PHLWORKSPACE, PHLMONITOR) { onLayoutChanged(); }));
        m_listeners.emplace_back(events.monitor.focused.listen([this](PHLMONITOR) { onLayoutChanged(); }));
    }

    if (!g_pEventLoopManager) {
        if (Log::logger)
            Log::logger->log(Log::ERR, "hyprsession", "event loop is missing; snapshots will not be timed");
        return;
    }

    m_periodic = makeShared<CEventLoopTimer>(std::chrono::seconds(intervalSec()), [](SP<CEventLoopTimer> self, void*) {
        if (g_session)
            g_session->onPeriodic(self);
    }, nullptr);
    m_debounce = makeShared<CEventLoopTimer>(std::chrono::seconds(2), [](SP<CEventLoopTimer> self, void*) {
        if (g_session)
            g_session->onDebounce(self);
    }, nullptr);
    m_restoreTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) {
        if (g_session)
            g_session->finishRestore();
    }, nullptr);
    m_offerTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer> self, void*) {
        if (g_session)
            g_session->onOfferTick(self);
    }, nullptr);
    g_pEventLoopManager->addTimer(m_periodic);
    g_pEventLoopManager->addTimer(m_debounce);
    g_pEventLoopManager->addTimer(m_restoreTimer);
    g_pEventLoopManager->addTimer(m_offerTimer);

    // A fresh compositor would otherwise snapshot the empty login over the saved layout
    // before the notification is answered. Config reload happens in a long-lived process.
    if (shouldOffer()) {
        m_suspendWrites = true;
        markOffered();
        g_pEventLoopManager->doLater([] {
            if (g_session)
                g_session->spawnOffer();
        });
    }

    if (configBool("plugin:hyprsession:restore_on_start", false)) {
        g_pEventLoopManager->doLater([] {
            if (g_session)
                g_session->restore();
        });
    }
}

void Session::shutdown() {
    if (m_stopped)
        return;
    m_stopped = true;
    if (m_offerPid > 0) {
        ::kill(m_offerPid, SIGTERM);
        int status = 0;
        ::waitpid(m_offerPid, &status, WNOHANG);
        m_offerPid = 0;
    }
    if (m_offerFd >= 0) {
        ::close(m_offerFd);
        m_offerFd = -1;
    }
    if (!m_restoreActive && !m_suspendWrites)
        flush();
    m_listeners.clear();
    if (g_pEventLoopManager) {
        if (m_periodic)
            g_pEventLoopManager->removeTimer(m_periodic);
        if (m_debounce)
            g_pEventLoopManager->removeTimer(m_debounce);
        if (m_restoreTimer)
            g_pEventLoopManager->removeTimer(m_restoreTimer);
        if (m_offerTimer)
            g_pEventLoopManager->removeTimer(m_offerTimer);
    }
    m_periodic.reset();
    m_debounce.reset();
    m_restoreTimer.reset();
    m_offerTimer.reset();
    reapChildren();
}

SDispatchResult Session::snapshotNow() {
    if (m_restoreActive)
        return {.success = false, .error = "restore in progress"};
    if (!flush())
        return {.success = false, .error = "snapshot write failed"};
    return {};
}

SDispatchResult Session::restore() {
    if (m_restoreActive)
        return {.success = false, .error = "restore already in progress"};

    std::string error;
    auto        text = readSnapshotFile(defaultSnapshotPath(), error);
    if (!text)
        return {.success = false, .error = error.empty() ? "no snapshot yet" : error};

    auto snapshot = fromJson(*text);
    if (!snapshot)
        return {.success = false, .error = "snapshot could not be read"};

    m_snapshot = std::move(*snapshot);
    m_restoreActive = true;
    m_suspendWrites = true;
    m_placed.clear();
    m_pending.clear();
    m_presented.clear();
    m_launched = 0;

    const auto open = openWindows();
    std::vector<LiveWindow> live;
    live.reserve(open.size());
    for (const auto& window : open)
        live.push_back(window.identity);

    const auto placements = matchWindows(m_snapshot.windows, live);
    std::unordered_set<size_t> matched;
    for (const auto& placement : placements) {
        matched.insert(placement.saved);
        if (placement.live < open.size())
            place(open[placement.live].window, placement.saved, false);
    }

    for (const auto& monitor : m_snapshot.monitors)
        applyMonitor(monitor, m_snapshot);
    focusSaved();

    const auto launches = planLaunches(m_snapshot, placements, splitSkipList(configString("plugin:hyprsession:skip_class")));
    for (const auto& launch : launches) {
        const auto spawned = spawnRequest(launch);
        if (!spawned.ok) {
            if (Log::logger)
                Log::logger->log(Log::ERR, "hyprsession", "launch {}: {}", launch.appId, spawned.error);
            continue;
        }
        ++m_launched;
        m_children.push_back(spawned.pid);
        m_pending.insert(m_pending.end(), launch.pending.begin(), launch.pending.end());
    }

    if (m_pending.empty())
        finishRestore();
    else
        armRestoreTimer();

    return {};
}

void Session::onLayoutChanged() {
    if (m_suspendWrites || m_stopped || !m_debounce)
        return;
    m_debounce->updateTimeout(std::chrono::seconds(2));
    if (g_pEventLoopManager)
        g_pEventLoopManager->scheduleRecalc();
}

void Session::onWindowOpened(PHLWINDOW window) {
    if (!m_restoreActive || !window)
        return;

    const auto& meta = window->metadata();
    const auto claim = claimPending(m_snapshot.windows, m_pending, LiveWindow{
        .appId = meta.appID(),
        .initialAppId = meta.initialAppID(),
        .initialTitle = meta.initialTitle(),
    });
    if (!claim)
        return;

    std::erase(m_pending, *claim);
    place(window, *claim, true);
    if (m_pending.empty())
        finishRestore();
}

void Session::onPeriodic(SP<CEventLoopTimer> timer) {
    reapChildren();
    if (!m_suspendWrites)
        flush();
    if (timer)
        timer->updateTimeout(std::chrono::seconds(intervalSec()));
}

void Session::onDebounce(SP<CEventLoopTimer> timer) {
    if (!m_suspendWrites)
        flush();
    if (timer)
        timer->updateTimeout(std::nullopt);
}

void Session::finishRestore() {
    if (!m_restoreActive)
        return;

    finishGroups();
    for (const auto& [index, reference] : m_placed) {
        (void)reference;
        present(index);
    }
    const int rebuilt = applySplits(m_snapshot, [&](size_t index) -> PHLWINDOW {
        const auto found = m_placed.find(index);
        if (found == m_placed.end())
            return nullptr;
        return found->second.lock();
    });
    for (const auto& [index, reference] : m_placed) {
        (void)reference;
        if (index >= m_snapshot.windows.size())
            continue;
        if (const auto window = m_placed.at(index).lock())
            applyFullscreen(window, m_snapshot.windows[index]);
    }
    focusSaved();

    const int placed   = static_cast<int>(m_placed.size());
    const int launched = m_launched;
    m_restoreActive    = false;
    m_suspendWrites    = false;
    m_pending.clear();
    m_placed.clear();
    m_presented.clear();
    m_snapshot = {};
    if (m_restoreTimer)
        m_restoreTimer->updateTimeout(std::nullopt);
    if (g_pEventLoopManager)
        g_pEventLoopManager->scheduleRecalc();

    flush();
    notify("hyprsession placed " + std::to_string(placed) + " windows, launched " + std::to_string(launched) + " apps, rebuilt " + std::to_string(rebuilt) + " splits");
}

bool Session::flush() {
    if (m_suspendWrites)
        return false;

    const auto path = defaultSnapshotPath();
    if (path.empty())
        return false;

    const auto payload = toJson(captureLayout());
    if (payload == m_lastWritten)
        return true;

    std::string error;
    if (!writeSnapshotFile(path, payload, error)) {
        if (Log::logger)
            Log::logger->log(Log::ERR, "hyprsession", "write snapshot: {}", error);
        return false;
    }
    m_lastWritten = payload;
    return true;
}

void Session::place(PHLWINDOW window, size_t index, bool focusNow) {
    if (!window || index >= m_snapshot.windows.size())
        return;

    const auto& record = m_snapshot.windows[index];
    moveToSavedWorkspace(window, record, m_snapshot);
    m_placed[index] = window;
    if (record.groupId.empty())
        present(index);
    else if (groupComplete(record.groupId)) {
        assemble(record.groupId);
        for (size_t member = 0; member < m_snapshot.windows.size(); ++member) {
            if (m_snapshot.windows[member].groupId == record.groupId)
                present(member);
        }
    }
    if (focusNow && record.focused)
        focusWindow(window);
}

void Session::present(size_t index) {
    if (!m_presented.insert(index).second)
        return;
    const auto found = m_placed.find(index);
    if (found == m_placed.end() || index >= m_snapshot.windows.size())
        return;
    if (auto window = found->second.lock())
        applyChrome(window, m_snapshot.windows[index]);
}

bool Session::groupComplete(const std::string& groupId) const {
    if (groupId.empty())
        return false;
    for (size_t index = 0; index < m_snapshot.windows.size(); ++index) {
        if (m_snapshot.windows[index].groupId != groupId)
            continue;
        if (!m_placed.contains(index))
            return false;
    }
    return true;
}

void Session::assemble(const std::string& groupId) {
    std::vector<GroupMember> members;
    for (size_t index = 0; index < m_snapshot.windows.size(); ++index) {
        const auto& record = m_snapshot.windows[index];
        if (record.groupId != groupId)
            continue;
        const auto found = m_placed.find(index);
        if (found == m_placed.end())
            continue;
        auto window = found->second.lock();
        if (!window)
            continue;
        members.push_back(GroupMember{
            .index   = record.groupIndex,
            .current = record.groupCurrent,
            .locked  = record.groupLocked,
            .window  = window,
        });
    }
    std::sort(members.begin(), members.end(), [](const GroupMember& left, const GroupMember& right) { return left.index < right.index; });
    assembleGroup(members);
}

void Session::finishGroups() {
    std::unordered_set<std::string> groupIds;
    for (const auto& [index, reference] : m_placed) {
        (void)reference;
        if (index < m_snapshot.windows.size() && !m_snapshot.windows[index].groupId.empty())
            groupIds.insert(m_snapshot.windows[index].groupId);
    }
    for (const auto& groupId : groupIds)
        assemble(groupId);
}

void Session::focusSaved() {
    for (const auto& [index, reference] : m_placed) {
        if (index >= m_snapshot.windows.size() || !m_snapshot.windows[index].focused)
            continue;
        if (auto window = reference.lock())
            focusWindow(window);
    }
}

void Session::armRestoreTimer() {
    if (!m_restoreTimer || !g_pEventLoopManager)
        return;
    m_restoreTimer->updateTimeout(std::chrono::seconds(restoreTimeoutSec()));
    g_pEventLoopManager->scheduleRecalc();
}

void Session::reapChildren() {
    for (auto child = m_children.begin(); child != m_children.end();) {
        int status = 0;
        const pid_t result = ::waitpid(*child, &status, WNOHANG);
        if (result == *child || (result < 0 && errno == ECHILD))
            child = m_children.erase(child);
        else
            ++child;
    }
}

bool Session::shouldOffer() const {
    if (compositorAgeSec() > 90 || alreadyOffered())
        return false;
    const auto snapshot = fromJson(m_lastWritten);
    return snapshot && !snapshot->windows.empty();
}

void Session::spawnOffer() {
    if (m_stopped || !m_suspendWrites)
        return;
    if (!commandRunnable("notify-send")) {
        m_suspendWrites = false;
        return;
    }

    int pipeFd[2] = {-1, -1};
    if (::pipe(pipeFd) != 0) {
        m_suspendWrites = false;
        return;
    }
    const int flags = ::fcntl(pipeFd[0], F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(pipeFd[0], F_SETFL, flags | O_NONBLOCK);

    std::vector<std::string> args{
        "notify-send",
        "--app-name=hyprsession",
        "--urgency=normal",
        "--expire-time=15000",
        "--action=restore=Restore",
        "--action=close=Close",
        "Restore Hyprland session?",
        "Restore puts the last snapshot back. Close leaves this login alone.",
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipeFd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipeFd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFd[1]);

    pid_t     spawned = 0;
    const int rc      = ::posix_spawnp(&spawned, args.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFd[1]);
    if (rc != 0) {
        ::close(pipeFd[0]);
        m_suspendWrites = false;
        if (Log::logger)
            Log::logger->log(Log::ERR, "hyprsession", "restore prompt failed: {}", std::strerror(rc));
        return;
    }

    m_offerPid     = spawned;
    m_offerFd      = pipeFd[0];
    m_offerOutput.clear();
    m_offerStarted = std::chrono::steady_clock::now();
    if (m_offerTimer && g_pEventLoopManager) {
        m_offerTimer->updateTimeout(std::chrono::milliseconds(200));
        g_pEventLoopManager->scheduleRecalc();
    }
}

void Session::onOfferTick(SP<CEventLoopTimer> timer) {
    if (m_offerFd >= 0) {
        char buffer[256];
        while (true) {
            const ssize_t n = ::read(m_offerFd, buffer, sizeof buffer);
            if (n > 0) {
                m_offerOutput.append(buffer, static_cast<size_t>(n));
                continue;
            }
            break;
        }
    }

    int         status = 0;
    const pid_t result = m_offerPid > 0 ? ::waitpid(m_offerPid, &status, WNOHANG) : -1;
    const bool  exited = result == m_offerPid || (result < 0 && errno == ECHILD);
    if (!exited && m_offerPid > 0 && std::chrono::steady_clock::now() - m_offerStarted > std::chrono::seconds(120))
        ::kill(m_offerPid, SIGTERM);
    if (!exited) {
        if (timer && g_pEventLoopManager) {
            timer->updateTimeout(std::chrono::milliseconds(200));
            g_pEventLoopManager->scheduleRecalc();
        }
        return;
    }
    finishOffer();
}

void Session::finishOffer() {
    if (m_offerFd >= 0) {
        ::close(m_offerFd);
        m_offerFd = -1;
    }
    m_offerPid = 0;
    if (m_offerTimer)
        m_offerTimer->updateTimeout(std::nullopt);
    if (g_pEventLoopManager)
        g_pEventLoopManager->scheduleRecalc();
    if (m_stopped)
        return;

    if (trimmedAction(m_offerOutput) == "restore") {
        const auto result = restore();
        if (!result.success)
            m_suspendWrites = false;
        return;
    }
    m_suspendWrites = false;
}

void Session::notify(const std::string& text) const {
    if (m_handle)
        HyprlandAPI::addNotification(m_handle, text, CHyprColor(0.35F, 0.75F, 0.95F, 1.0F), 4000);
}

} // namespace hyprsession
