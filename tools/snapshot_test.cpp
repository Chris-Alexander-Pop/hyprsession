#include "snapshot.hpp"
#include "store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition)
        return;
    std::cerr << "fail: " << message << '\n';
    ++g_failures;
}

hyprsession::WindowRecord window(std::string app, std::string title, int pid, std::vector<std::string> argv = {}) {
    hyprsession::WindowRecord record;
    record.appId        = app;
    record.initialAppId = app;
    record.title        = title;
    record.initialTitle = title;
    record.pid          = pid;
    record.argv         = std::move(argv);
    record.cwd          = "/tmp/project";
    record.workspace    = "1";
    record.monitor      = "eDP-1";
    return record;
}

void testRoundTrip() {
    hyprsession::Snapshot snapshot;
    snapshot.monitors.push_back(hyprsession::MonitorRecord{
        .name                = "eDP-1",
        .description         = "panel",
        .activeWorkspace     = "2",
        .activeSpecial       = "special:music",
        .focusedAppId        = "kitty",
        .focusedInitialTitle = "shell",
    });
    snapshot.workspaces.push_back(hyprsession::WorkspaceRecord{
        .address     = "special:music",
        .displayName = "music",
        .type        = "special",
        .monitor     = "eDP-1",
        .numberedId  = 0,
    });

    auto kitty = window("kitty", "shell", 40, {"kitty"});
    kitty.floating     = true;
    kitty.hasBox       = true;
    kitty.box          = {10, 20, 800, 600};
    kitty.groupId      = "g0";
    kitty.groupIndex   = 1;
    kitty.groupCurrent = true;
    kitty.groupLocked  = true;
    kitty.focused      = true;
    kitty.fullscreen   = 2;
    kitty.pinned       = true;
    snapshot.windows.push_back(kitty);
    snapshot.windows.push_back(window("firefox", "docs", 40, {"firefox"}));

    hyprsession::SplitNode left{.leaf = true, .window = 0};
    hyprsession::SplitNode right{.leaf = true, .window = 1};
    hyprsession::SplitNode root;
    root.splitTop = true;
    root.ratio    = 1.25;
    root.children = {left, right};
    snapshot.splits.push_back(hyprsession::WorkspaceSplit{.workspace = "1", .root = root});

    const auto text    = hyprsession::toJson(snapshot);
    const auto parsed  = hyprsession::fromJson(text);
    expect(parsed.has_value(), "round trip parses");
    if (!parsed)
        return;
    expect(hyprsession::toJson(*parsed) == text, "round trip is stable");
    expect(parsed->monitors[0].activeSpecial == "special:music", "special workspace kept");
    expect(parsed->windows[0].box.w == 800, "floating box kept");
    expect(parsed->windows[0].groupLocked && parsed->windows[0].focused, "group and focus kept");
    expect(parsed->windows[1].pid == 40, "shared pid kept");
    expect(parsed->splits.size() == 1 && parsed->splits[0].root.splitTop, "split direction kept");
    expect(parsed->splits[0].root.children.size() == 2 && parsed->splits[0].root.children[0].window == 0, "split leaves kept");
    expect(parsed->splits[0].root.ratio == 1.25, "split ratio kept");
    const auto older = hyprsession::fromJson("{\"version\":1,\"windows\":[]}\n");
    expect(older && older->splits.empty(), "a snapshot without splits still loads");
    expect(!hyprsession::fromJson("{\"version\":2}").has_value(), "unknown version rejected");
    expect(!hyprsession::fromJson("not json").has_value(), "garbage rejected");
}

void testMatch() {
    const std::vector<hyprsession::WindowRecord> saved{
        window("kitty", "a", 1, {"kitty"}),
        window("kitty", "b", 2, {"kitty"}),
        window("firefox", "docs", 3, {"firefox"}),
    };
    const std::vector<hyprsession::LiveWindow> live{
        {.appId = "kitty", .initialAppId = "kitty", .initialTitle = "b"},
        {.appId = "kitty", .initialAppId = "kitty", .initialTitle = "a"},
        {.appId = "firefox", .initialAppId = "firefox", .initialTitle = "other"},
    };

    const auto placements = hyprsession::matchWindows(saved, live);
    expect(placements.size() == 3, "three windows match");
    expect(placements[0].saved == 0 && placements[0].live == 1, "kitty a matches its title");
    expect(placements[1].saved == 1 && placements[1].live == 0, "kitty b matches its title");
    expect(placements[2].saved == 2 && placements[2].live == 2, "firefox falls back to class");

    const std::vector<hyprsession::LiveWindow> twins{
        {.appId = "kitty", .initialAppId = "kitty", .initialTitle = "same"},
        {.appId = "kitty", .initialAppId = "kitty", .initialTitle = "same"},
    };
    const std::vector<hyprsession::WindowRecord> twinSaved{
        window("kitty", "same", 1),
        window("kitty", "same", 2),
    };
    const auto ordered = hyprsession::matchWindows(twinSaved, twins);
    expect(ordered.size() == 2, "identical titles both match");
    expect(ordered[0].live == 0 && ordered[1].live == 1, "identical titles stay in snapshot order");
}

void testLaunches() {
    hyprsession::Snapshot snapshot;
    snapshot.windows.push_back(window("firefox", "one", 10, {"firefox"}));
    snapshot.windows.push_back(window("firefox", "two", 10, {"firefox"}));
    snapshot.windows.push_back(window("kitty", "a", 11, {"kitty"}));
    snapshot.windows.push_back(window("kitty", "b", 12, {"kitty"}));
    auto portal = window("xdg-desktop-portal-gtk", "portal", 13, {"xdg-desktop-portal-gtk"});
    snapshot.windows.push_back(portal);
    auto flatpak = window("org.mozilla.firefox", "web", 14, {"/usr/bin/bwrap", "--args"});
    snapshot.windows.push_back(flatpak);
    snapshot.windows.push_back(window("btop", "top", 0, {}));

    const auto noneMatched = hyprsession::planLaunches(snapshot, {}, {});
    expect(noneMatched.size() == 4, "unmatched pids launch, portal and empty argv do not");

    bool sawFirefox = false;
    bool sawFlatpak = false;
    int  kittySpawns = 0;
    for (const auto& launch : noneMatched) {
        if (launch.appId == "firefox") {
            sawFirefox = true;
            expect(launch.pending.size() == 2, "firefox windows share one spawn");
            expect(launch.argv.size() == 1 && launch.argv[0] == "firefox", "firefox argv kept");
        }
        if (launch.appId == "kitty")
            ++kittySpawns;
        if (launch.appId == "org.mozilla.firefox") {
            sawFlatpak = true;
            expect(launch.argv.size() == 3 && launch.argv[0] == "flatpak" && launch.argv[2] == "org.mozilla.firefox", "bwrap becomes flatpak run");
        }
    }
    expect(sawFirefox && sawFlatpak && kittySpawns == 2, "firefox once, each kitty, flatpak rewritten");

    const auto partial = hyprsession::planLaunches(snapshot, {hyprsession::Placement{.saved = 0, .live = 0}}, {});
    bool relaunchedFirefox = false;
    for (const auto& launch : partial) {
        if (launch.appId == "firefox")
            relaunchedFirefox = true;
    }
    expect(!relaunchedFirefox, "a running firefox process is not launched again");

    const auto extra = hyprsession::planLaunches(snapshot, {}, {"kitty"});
    for (const auto& launch : extra)
        expect(launch.appId != "kitty", "skip list drops kitty");

    expect(hyprsession::skippedClass("Hyprland", {}), "hyprland is skipped");
    expect(hyprsession::skippedClass("polkit-gnome", {}), "polkit agent is skipped");
    expect(hyprsession::splitSkipList(" kitty, firefox ").size() == 2, "skip list trims tokens");
}

void testClaim() {
    const std::vector<hyprsession::WindowRecord> saved{
        window("kitty", "a", 1),
        window("kitty", "b", 2),
    };
    const std::vector<size_t> pending{0, 1};
    const auto claimed = hyprsession::claimPending(saved, pending, {.appId = "kitty", .initialAppId = "kitty", .initialTitle = "b"});
    expect(claimed == 1, "a new window claims the matching title");
    const auto fallback = hyprsession::claimPending(saved, pending, {.appId = "kitty", .initialAppId = "kitty", .initialTitle = ""});
    expect(fallback == 0, "an empty title claims the first pending window of that class");
}

void testResume() {
    const std::string chat = "11111111-2222-4333-8444-555555555555";
    const std::string store = "/tmp/user/.config/cursor/chats/0123456789abcdef/" + chat + "/store.db";
    const auto id = hyprsession::cursorChatId(store);
    expect(id && *id == chat, "chat id comes from the open store.db");
    expect(!hyprsession::cursorChatId(store + "-wal").has_value(), "a wal sidecar is not the chat");
    if (!id)
        return;

    const auto resumed = hyprsession::relaunchArgv("foot", "/tmp/work", *id);
    expect(resumed.has_value(), "an agent terminal gets a relaunch line");
    if (resumed) {
        const std::vector<std::string> expected{"foot", "-D", "/tmp/work", "agent", "--resume", *id, "--workspace", "/tmp/work"};
        expect(*resumed == expected, "agent relaunch resumes that chat in that directory");
    }

    const auto idle = hyprsession::relaunchArgv("foot", "/tmp/work", "");
    expect(idle && *idle == std::vector<std::string>{"foot", "-D", "/tmp/work"}, "an idle shell comes back in its directory");
    const auto running = hyprsession::relaunchArgv("foot", "/tmp", "", {"btop"});
    expect(running && running->size() == 4 && running->back() == "btop", "a foreground command is kept");
    const auto editor = hyprsession::relaunchArgv("Cursor", "/tmp/work", "");
    expect(editor && *editor == std::vector<std::string>{"cursor"}, "cursor is launched once by name");
    expect(!hyprsession::relaunchArgv("foot", "/", "").has_value(), "a terminal with no directory keeps its original command");
    expect(!hyprsession::relaunchArgv("kitty", "/tmp", "").has_value(), "other apps keep their original command");
}

void testStore() {
    const auto dir  = std::filesystem::temp_directory_path() / "hyprsession-test";
    const auto path = dir / "nested" / "hyprsession.json";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::string error;
    const std::string payload = "{\"version\":1}\n";
    expect(hyprsession::writeSnapshotFile(path, payload, error), "atomic write succeeds: " + error);
    const auto read = hyprsession::readSnapshotFile(path, error);
    expect(read && *read == payload, "atomic read matches");
    expect(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")), "temp file is gone");

    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
    const auto mode = std::filesystem::status(path).permissions();
    expect((mode & std::filesystem::perms::owner_read) != std::filesystem::perms::none, "owner can read the snapshot");
    expect((mode & std::filesystem::perms::group_read) == std::filesystem::perms::none, "group cannot read the snapshot");

    const auto missing = hyprsession::readSnapshotFile(dir / "nope.json", error);
    expect(!missing && error.empty(), "a missing snapshot is not an error");
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    testRoundTrip();
    testMatch();
    testLaunches();
    testClaim();
    testResume();
    testStore();
    if (g_failures != 0) {
        std::cerr << g_failures << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
