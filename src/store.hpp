#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hyprsession {

std::filesystem::path defaultSnapshotPath();

bool writeSnapshotFile(const std::filesystem::path& path, std::string_view payload, std::string& error);

// Missing file: nullopt and an empty error. A read failure sets error.
std::optional<std::string> readSnapshotFile(const std::filesystem::path& path, std::string& error);

} // namespace hyprsession
