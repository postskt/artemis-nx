#include "AssetManager.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <system_error>

namespace artc {

AssetManager::AssetManager(const Config& config, std::filesystem::path root,
                           WarningSink warning_sink)
    : config_(config),
      root_(std::move(root)),
      warning_sink_(std::move(warning_sink)) {
    if (!warning_sink_) {
        warning_sink_ = [](const std::string& message) {
            std::cerr << "[asset] " << message << '\n';
        };
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root_, ec)) {
        warn("game root is not a directory: " + root_.string());
        return;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             root_, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        std::error_code rel_ec;
        const auto relative = std::filesystem::relative(it->path(), root_, rel_ec);
        if (rel_ec) {
            rel_ec.clear();
            continue;
        }
        index_[lower_ascii(relative.generic_string())] = relative.generic_string();
    }
}

std::string AssetManager::lower_ascii(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string AssetManager::normalize_relative(const std::string& value) {
    std::string result = value;
    std::replace(result.begin(), result.end(), '\\', '/');
    while (result.rfind("./", 0) == 0) result.erase(0, 2);
    while (!result.empty() && result.front() == '/') result.erase(result.begin());
    return result;
}

bool AssetManager::exists(const std::string& relative) const {
    const std::string normalized = normalize_relative(relative);
    const std::string lower = lower_ascii(normalized);
    if (index_.find(lower) != index_.end()) return true;
    std::error_code ec;
    if (std::filesystem::is_regular_file(root_ / normalized, ec)) return true;
    std::ifstream probe(root_ / normalized, std::ios::binary);
    return probe.good();
}

std::optional<std::string> AssetManager::resolve(const std::string& relative) const {
    const std::string normalized = normalize_relative(relative);
    const std::string lower = lower_ascii(normalized);
    auto it = index_.find(lower);
    if (it != index_.end()) {
        return it->second;
    }
    // Some console filesystems expose files through open/read but not through
    // std::filesystem directory iteration.  Try the lower-case path directly.
    std::error_code ec;
    const auto direct = root_ / lower;
    if (std::filesystem::is_regular_file(direct, ec)) return lower;
    std::ifstream probe(direct, std::ios::binary);
    if (probe.good()) return lower;
    warn("missing resource: " + normalized);
    return std::nullopt;
}

std::optional<std::string> AssetManager::resolve_magic(const std::string& magic_path) const {
    return resolve(config_.resolve_magic(magic_path));
}

std::optional<std::string> AssetManager::read_text(const std::string& relative) const {
    auto actual = resolve(relative);
    if (!actual) return std::nullopt;
    const auto path = root_ / *actual;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        warn("cannot open text resource: " + *actual);
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || size > 16 * 1024 * 1024) {
        warn("text resource too large: " + *actual);
        return std::nullopt;
    }
    in.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in) {
        warn("cannot read text resource: " + *actual);
        return std::nullopt;
    }
    return data;
}

void AssetManager::warn(const std::string& message) const {
    last_warning_ = message;
    if (warnings_.size() < 256) warnings_.push_back(message);
    if (warning_sink_) warning_sink_(message);
}

}  // namespace artc
