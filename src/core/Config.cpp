#include "Config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace artc {

namespace {
std::string require_string(const nlohmann::json& j, const char* key,
                           const std::string& context) {
    if (!j.contains(key) || !j.at(key).is_string()) {
        throw std::runtime_error(context + ": missing string field '" + key + "'");
    }
    return j.at(key).get<std::string>();
}

int require_int(const nlohmann::json& j, const char* key,
                const std::string& context) {
    if (!j.contains(key) || !j.at(key).is_number_integer()) {
        throw std::runtime_error(context + ": missing integer field '" + key + "'");
    }
    return j.at(key).get<int>();
}
}  // namespace

Config Config::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open runner config: " + path.string());
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("invalid runner config JSON: " + std::string(e.what()));
    }
    Config c;
    c.source_path_ = path;
    c.window_title_ = require_string(j, "window_title", path.string());
    c.logical_width_ = require_int(j, "logical_width", path.string());
    c.logical_height_ = require_int(j, "logical_height", path.string());
    c.entry_script_ = require_string(j, "entry_script", path.string());
    c.default_font_ = require_string(j, "default_font", path.string());

    if (j.contains("script_base") && j["script_base"].is_string()) {
        c.script_base_ = j["script_base"].get<std::string>();
    }

    if (j.contains("path_mappings") && j["path_mappings"].is_object()) {
        for (auto it = j["path_mappings"].begin(); it != j["path_mappings"].end(); ++it) {
            if (it.value().is_string()) {
                c.path_mappings_[it.key()] = it.value().get<std::string>();
            }
        }
    }

    if (j.contains("ui") && j["ui"].is_object() && j["ui"].contains("textbox") &&
        j["ui"]["textbox"].is_string()) {
        c.ui_textbox_ = j["ui"]["textbox"].get<std::string>();
    }

    if (j.contains("font_layout") && j["font_layout"].is_object()) {
        c.font_layout_ = j["font_layout"];
    }

    if (j.contains("extensions") && j["extensions"].is_object()) {
        const auto& e = j["extensions"];
        if (e.contains("image") && e["image"].is_string()) {
            c.image_ext_ = e["image"].get<std::string>();
        }
    }

    if (j.contains("color_tones") && j["color_tones"].is_object()) {
        for (auto it = j["color_tones"].begin(); it != j["color_tones"].end(); ++it) {
            if (it.value().is_string()) {
                c.color_tones_[it.key()] = it.value().get<std::string>();
            }
        }
    }

    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& a = j["audio"];
        if (a.contains("sound_ext") && a["sound_ext"].is_string()) {
            c.audio_.sound_ext = a["sound_ext"].get<std::string>();
        }
        if (a.contains("voice_channels") && a["voice_channels"].is_array()) {
            c.audio_.voice_channels.clear();
            for (const auto& value : a["voice_channels"]) {
                if (value.is_number_integer()) c.audio_.voice_channels.push_back(value.get<int>());
            }
        }
        if (a.contains("se_channels") && a["se_channels"].is_array()) {
            c.audio_.se_channels.clear();
            for (const auto& value : a["se_channels"]) {
                if (value.is_number_integer()) c.audio_.se_channels.push_back(value.get<int>());
            }
        }
        if (a.contains("bgm_intro_suffix") && a["bgm_intro_suffix"].is_string()) {
            c.audio_.bgm_intro_suffix = a["bgm_intro_suffix"].get<std::string>();
        }
        if (a.contains("bgm_loop_suffix") && a["bgm_loop_suffix"].is_string()) {
            c.audio_.bgm_loop_suffix = a["bgm_loop_suffix"].get<std::string>();
        }
    }

    if (j.contains("extra_bgm") && j["extra_bgm"].is_object()) {
        for (auto it = j["extra_bgm"].begin(); it != j["extra_bgm"].end(); ++it) {
            if (!it.value().is_object()) continue;
            BgmEntry entry;
            entry.name = it.key();
            if (it.value().contains("intro") && it.value()["intro"].is_string()) {
                entry.intro = it.value()["intro"].get<std::string>();
            }
            if (it.value().contains("loop") && it.value()["loop"].is_string()) {
                entry.loop = it.value()["loop"].get<std::string>();
            }
            if (it.value().contains("intro_exists") && it.value()["intro_exists"].is_boolean()) {
                entry.intro_exists = it.value()["intro_exists"].get<bool>();
            }
            c.extra_bgm_[it.key()] = std::move(entry);
        }
    }

    if (j.contains("fgpos_table") && j["fgpos_table"].is_object()) {
        for (auto head = j["fgpos_table"].begin(); head != j["fgpos_table"].end(); ++head) {
            if (!head.value().is_object()) continue;
            auto& by_key = c.fgpos_[head.key()];
            for (auto item = head.value().begin(); item != head.value().end(); ++item) {
                if (!item.value().is_array() || item.value().size() < 2) continue;
                if (!item.value()[0].is_number_integer() || !item.value()[1].is_number_integer()) {
                    continue;
                }
                by_key[item.key()] = FgPosition{
                    item.value()[0].get<int>(),
                    item.value()[1].get<int>(),
                };
            }
        }
    }

    if (j.contains("fg_size_scales") && j["fg_size_scales"].is_object()) {
        for (auto it = j["fg_size_scales"].begin(); it != j["fg_size_scales"].end(); ++it) {
            if (it.value().is_number()) {
                c.fg_size_scales_[it.key()] = it.value().get<double>();
            }
        }
    }

    if (j.contains("scripts") && j["scripts"].is_array()) {
        for (const auto& value : j["scripts"]) {
            if (value.is_string()) c.scripts_.push_back(value.get<std::string>());
        }
    }

    if (c.entry_script_.empty() || c.script_base_.empty()) {
        throw std::runtime_error("runner config has empty entry_script/script_base");
    }
    return c;
}

std::optional<FgPosition> Config::fg_position(const std::string& head,
                                              const std::string& key) const {
    auto h = fgpos_.find(head);
    if (h == fgpos_.end()) return std::nullopt;
    auto k = h->second.find(key);
    if (k == h->second.end()) return std::nullopt;
    return k->second;
}

double Config::fg_scale(const std::string& size_key) const {
    auto it = fg_size_scales_.find(size_key);
    return it == fg_size_scales_.end() ? 1.0 : it->second;
}

const BgmEntry* Config::bgm(const std::string& logical_name) const {
    auto it = extra_bgm_.find(logical_name);
    return it == extra_bgm_.end() ? nullptr : &it->second;
}

std::string Config::join(const std::string& base, const std::string& relative) const {
    if (base.empty()) return relative;
    if (relative.empty()) return base;
    std::string out = base;
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    std::size_t pos = 0;
    while (pos < relative.size() && (relative[pos] == '/' || relative[pos] == '\\')) ++pos;
    out += relative.substr(pos);
    return out;
}

std::string Config::resolve_magic(const std::string& value) const {
    if (value.empty() || value.front() != ':') return value;
    std::string best;
    std::string result;
    for (const auto& [prefix, base] : path_mappings_) {
        if (prefix.empty() || prefix.front() != ':') continue;
        if (value.compare(0, prefix.size(), prefix) == 0 && prefix.size() > best.size()) {
            best = prefix;
            result = join(base, value.substr(prefix.size()));
        }
    }
    return best.empty() ? value : result;
}

std::string Config::script_path(const std::string& name) const {
    std::string value = name;
    if (value.size() >= 4 && value.substr(value.size() - 4) == ".ast") {
        value = value.substr(0, value.size() - 4);
    }
    return join(script_base_, value + ".ast");
}

}  // namespace artc
