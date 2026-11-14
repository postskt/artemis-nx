#include "Gallery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace artc {

void Gallery::scan(const Config&, const AssetManager& assets) {
    items_.clear();
    const auto thumb_dir = assets.root() / "image" / "xga" / "thumb";
    std::error_code ec;
    if (!std::filesystem::is_directory(thumb_dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(
             thumb_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (filename.size() < 7 || filename.substr(0, 2) != "ev" ||
            filename.substr(filename.size() - 4) != ".png") {
            continue;
        }
        const std::string id = filename.substr(0, filename.size() - 4);
        const std::string full_dir = "image/xga/ev/";
        std::string full = full_dir + id + ".png";
        if (!assets.exists(full)) {
            full = full_dir + id + "a.png";
        }
        if (!assets.exists(full)) continue;
        items_.push_back(GalleryItem{
            id,
            "image/xga/thumb/" + filename,
            full,
            false,
        });
    }
    std::sort(items_.begin(), items_.end(),
              [](const GalleryItem& a, const GalleryItem& b) { return a.id < b.id; });

    // Some RomFS implementations do not enumerate directories reliably.  Fall
    // back to the conventional evNN thumbnail names, probing through the
    // AssetManager (which has its own direct-path fallback).
    if (items_.empty()) {
        for (int number = 1; number <= 30; ++number) {
            char name[16]{};
            std::snprintf(name, sizeof(name), "ev%02d", number);
            const std::string id = name;
            const std::string thumb = "image/xga/thumb/" + id + ".png";
            if (!assets.exists(thumb)) continue;
            std::string full = "image/xga/ev/" + id + ".png";
            if (!assets.exists(full)) full = "image/xga/ev/" + id + "a.png";
            if (!assets.exists(full)) continue;
            items_.push_back(GalleryItem{id, thumb, full, false});
        }
    }
}

void Gallery::unlock_for_asset(const std::string& relative) {
    std::string path = relative;
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto marker = path.find("/ev/");
    if (marker == std::string::npos) return;
    const std::string filename = path.substr(marker + 4);
    const auto dot = filename.find_last_of('.');
    std::string id = dot == std::string::npos ? filename : filename.substr(0, dot);
    if (!id.empty() && std::isalpha(static_cast<unsigned char>(id.back()))) id.pop_back();
    if (!id.empty()) unlocked_.insert(id);
}

bool Gallery::load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    try {
        nlohmann::json data;
        input >> data;
        if (data.is_array()) {
            for (const auto& value : data) {
                if (value.is_string()) unlocked_.insert(value.get<std::string>());
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool Gallery::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    nlohmann::json data = nlohmann::json::array();
    for (const auto& item : items_) {
        if (is_unlocked(item)) data.push_back(item.id);
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << data.dump(2) << "\n";
    return output.good();
}

}  // namespace artc
