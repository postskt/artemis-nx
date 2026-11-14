#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace artc {

struct FgPosition {
    int x = 0;
    int y = 0;
};

struct BgmEntry {
    std::string name;
    std::string intro;
    std::string loop;
    bool intro_exists = false;
};

struct AudioLayout {
    std::string sound_ext = ".ogg";
    std::vector<int> voice_channels{0};
    std::vector<int> se_channels{1, 2, 3, 4, 5, 6, 7};
    std::string bgm_intro_suffix = "_a";
    std::string bgm_loop_suffix = "_b";
};

class Config {
public:
    static Config load(const std::filesystem::path& path);

    const std::string& window_title() const { return window_title_; }
    int logical_width() const { return logical_width_; }
    int logical_height() const { return logical_height_; }
    const std::string& entry_script() const { return entry_script_; }
    const std::string& script_base() const { return script_base_; }
    const std::string& default_font() const { return default_font_; }
    const std::string& ui_textbox() const { return ui_textbox_; }
    const std::map<std::string, std::string>& path_mappings() const { return path_mappings_; }
    const std::unordered_map<std::string, BgmEntry>& extra_bgm() const { return extra_bgm_; }
    const AudioLayout& audio() const { return audio_; }
    const nlohmann::json& font_layout() const { return font_layout_; }
    const std::vector<std::string>& scripts() const { return scripts_; }
    const std::string& image_ext() const { return image_ext_; }
    const std::map<std::string, std::string>& color_tones() const { return color_tones_; }

    std::optional<FgPosition> fg_position(const std::string& head,
                                          const std::string& key) const;
    double fg_scale(const std::string& size_key) const;
    const BgmEntry* bgm(const std::string& logical_name) const;

    // Resolve a magic path such as ":bg/" or ":fg/aki/no/" to a path relative
    // to the game root.  Unknown prefixes are returned unchanged.
    std::string resolve_magic(const std::string& value) const;
    std::string join(const std::string& base, const std::string& relative) const;
    std::string script_path(const std::string& name) const;

    const std::filesystem::path& source_config_path() const { return source_path_; }

private:
    std::string window_title_;
    int logical_width_ = 0;
    int logical_height_ = 0;
    std::string entry_script_;
    std::string script_base_ = "script/";
    std::string default_font_;
    std::string ui_textbox_;
    std::map<std::string, std::string> path_mappings_;
    std::unordered_map<std::string, BgmEntry> extra_bgm_;
    std::unordered_map<std::string, std::unordered_map<std::string, FgPosition>> fgpos_;
    std::unordered_map<std::string, double> fg_size_scales_;
    AudioLayout audio_;
    nlohmann::json font_layout_;
    std::vector<std::string> scripts_;
    std::string image_ext_ = ".png";
    std::map<std::string, std::string> color_tones_;
    std::filesystem::path source_path_;
};

}  // namespace artc
