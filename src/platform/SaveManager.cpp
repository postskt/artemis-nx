#include "SaveManager.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace artc {

namespace {

std::string now_string() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y/%m/%d %H:%M");
    return output.str();
}

bool read_json(const std::filesystem::path& path, nlohmann::json& value) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    try {
        input >> value;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

SaveManager::SaveManager(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
}

bool SaveManager::valid_slot(int slot) {
    return slot >= 1 && slot <= kSlotCount;
}

std::filesystem::path SaveManager::json_path(int slot) const {
    return directory_ / ("slot_" + std::to_string(slot) + ".json");
}

std::filesystem::path SaveManager::thumbnail_path(int slot) const {
    return directory_ / ("slot_" + std::to_string(slot) + ".png");
}

std::array<SaveSlotInfo, SaveManager::kSlotCount> SaveManager::list() const {
    std::array<SaveSlotInfo, kSlotCount> result{};
    for (int slot = 1; slot <= kSlotCount; ++slot) {
        SaveSlotInfo& info = result[slot - 1];
        info.slot = slot;
        nlohmann::json root;
        if (!read_json(json_path(slot), root)) continue;
        info.exists = true;
        info.title = root.value("title", "");
        info.saved_at = root.value("saved_at", "");
        info.script = root.value("script", "");
        info.thumbnail = thumbnail_path(slot).string();
        if (info.title.empty()) info.title = info.script;
    }
    return result;
}

std::optional<nlohmann::json> SaveManager::load(int slot) const {
    if (!valid_slot(slot)) return std::nullopt;
    nlohmann::json root;
    if (!read_json(json_path(slot), root)) return std::nullopt;
    if (!root.contains("state") || !root["state"].is_object()) return std::nullopt;
    return root["state"];
}

bool SaveManager::save(int slot, const nlohmann::json& state,
                       const std::string& thumbnail) {
    if (!valid_slot(slot) || !state.is_object()) return false;
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    nlohmann::json root = {
        {"version", 1},
        {"slot", slot},
        {"saved_at", now_string()},
        {"title", state.value("title", "")},
        {"script", state.value("script", "")},
        {"state", state},
    };
    if (!thumbnail.empty()) root["thumbnail"] = thumbnail;

    std::ofstream output(json_path(slot), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << std::setw(2) << root << "\n";
    return output.good();
}

bool SaveManager::erase(int slot) {
    if (!valid_slot(slot)) return false;
    std::error_code ec;
    std::filesystem::remove(json_path(slot), ec);
    ec.clear();
    std::filesystem::remove(thumbnail_path(slot), ec);
    return true;
}

int SaveManager::latest_slot() const {
    int latest = 0;
    std::filesystem::file_time_type latest_time{};
    for (int slot = 1; slot <= kSlotCount; ++slot) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(json_path(slot), ec)) continue;
        const auto time = std::filesystem::last_write_time(json_path(slot), ec);
        if (ec) continue;
        if (latest == 0 || time > latest_time) {
            latest = slot;
            latest_time = time;
        }
    }
    return latest;
}

}  // namespace artc
