#include "store.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <system_error>

namespace hyprsession {
namespace {

bool writeAll(int fd, std::string_view payload) {
    while (!payload.empty()) {
        const ssize_t wrote = ::write(fd, payload.data(), payload.size());
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        payload.remove_prefix(static_cast<size_t>(wrote));
    }
    return true;
}

} // namespace

std::filesystem::path defaultSnapshotPath() {
    std::filesystem::path base;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        base = state;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::filesystem::path(home) / ".local" / "state";
    else
        return {};
    return base / "hypr" / "hyprsession.json";
}

bool writeSnapshotFile(const std::filesystem::path& path, std::string_view payload, std::string& error) {
    if (path.empty()) {
        error = "snapshot path is empty";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    const int  fd  = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }

    const bool wrote = writeAll(fd, payload);
    if (wrote && ::fchmod(fd, 0600) != 0)
        error = std::strerror(errno);
    if (wrote && ::fsync(fd) != 0)
        error = std::strerror(errno);
    if (::close(fd) != 0 && error.empty())
        error = std::strerror(errno);
    if (!wrote || !error.empty()) {
        ::unlink(tmp.c_str());
        if (error.empty())
            error = "short write";
        return false;
    }

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }

    const int dirfd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd >= 0) {
        ::fsync(dirfd);
        ::close(dirfd);
    }
    return true;
}

std::optional<std::string> readSnapshotFile(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::ifstream in(path);
    if (!in) {
        if (errno != ENOENT)
            error = std::strerror(errno);
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace hyprsession
