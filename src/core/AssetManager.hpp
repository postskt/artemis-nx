#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.hpp"

namespace artc {

class AssetManager {
public:
    using WarningSink = std::function<void(const std::string&)>;

    AssetManager(const Config& config, std::filesystem::path root,
                 WarningSink warning_sink = {});

    const std::filesystem::path& root() const { return root_; }
    const Config& config() const { return config_; }

    bool exists(const std::string& relative) const;
    std::optional<std::string> resolve(const std::string& relative) const;
    std::optional<std::string> read_text(const std::string& relative) const;
    std::optional<std::string> resolve_magic(const std::string& magic_path) const;

    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::string& last_warning() const { return last_warning_; }

    void warn(const std::string& message) const;

private:
    const Config& config_;
    std::filesystem::path root_;
    WarningSink warning_sink_;
    std::unordered_map<std::string, std::string> index_;
    mutable std::vector<std::string> warnings_;
    mutable std::string last_warning_;

    static std::string lower_ascii(const std::string& value);
    static std::string normalize_relative(const std::string& value);
};

}  // namespace artc
