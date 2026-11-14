#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "AssetManager.hpp"
#include "Config.hpp"

namespace artc {

struct GalleryItem {
    std::string id;
    std::string thumb;
    std::string full;
    bool unlocked = false;
};

class Gallery {
public:
    void scan(const Config& config, const AssetManager& assets);
    void unlock_for_asset(const std::string& relative);
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    const std::vector<GalleryItem>& items() const { return items_; }
    bool is_unlocked(const GalleryItem& item) const {
        return unlocked_.find(item.id) != unlocked_.end();
    }

private:
    std::vector<GalleryItem> items_;
    std::unordered_set<std::string> unlocked_;
};

}  // namespace artc
