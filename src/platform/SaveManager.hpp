#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace artc {

struct SaveSlotInfo {
    int slot = 0;
    bool exists = false;
    std::string title;
    std::string saved_at;
    std::string script;
    std::string thumbnail;
};

class SaveManager {
public:
    static constexpr int kSlotCount = 8;

    explicit SaveManager(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return directory_; }
    std::array<SaveSlotInfo, kSlotCount> list() const;

    std::optional<nlohmann::json> load(int slot) const;
    bool save(int slot, const nlohmann::json& state, const std::string& thumbnail = {});
    bool erase(int slot);
    int latest_slot() const;

    std::filesystem::path json_path(int slot) const;
    std::filesystem::path thumbnail_path(int slot) const;

private:
    std::filesystem::path directory_;
    static bool valid_slot(int slot);
};

}  // namespace artc
