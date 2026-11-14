#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace artc {

struct Command {
    std::string name;
    std::vector<nlohmann::json> args;
    std::unordered_map<std::string, nlohmann::json> kwargs;

    bool has(const std::string& key) const { return kwargs.find(key) != kwargs.end(); }

    std::string str(const std::string& key, const std::string& fallback = {}) const {
        auto it = kwargs.find(key);
        return it != kwargs.end() && it->second.is_string()
                   ? it->second.get<std::string>()
                   : fallback;
    }

    int integer(const std::string& key, int fallback = 0) const {
        auto it = kwargs.find(key);
        if (it == kwargs.end()) return fallback;
        if (it->second.is_number_integer()) return it->second.get<int>();
        if (it->second.is_number()) return static_cast<int>(it->second.get<double>());
        if (it->second.is_string()) {
            try {
                return std::stoi(it->second.get<std::string>());
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    }

    double number(const std::string& key, double fallback = 0.0) const {
        auto it = kwargs.find(key);
        if (it == kwargs.end()) return fallback;
        if (it->second.is_number()) return it->second.get<double>();
        if (it->second.is_string()) {
            try {
                return std::stod(it->second.get<std::string>());
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    }

    bool boolean(const std::string& key, bool fallback = false) const {
        auto it = kwargs.find(key);
        return it != kwargs.end() && it->second.is_boolean()
                   ? it->second.get<bool>()
                   : fallback;
    }
};

struct Block {
    int index = 0;
    int line = 0;
    std::vector<Command> commands;

    bool has_command(const std::string& name) const {
        for (const auto& command : commands) {
            if (command.name == name) return true;
        }
        return false;
    }
};

struct Voice {
    std::string file;
    std::string ch;
};

struct TextUnit {
    std::optional<std::string> speaker;
    std::string text;
    std::vector<Voice> voices;
    std::vector<std::string> select_options;
};

struct LabelTarget {
    int block = 0;    // 1-based block
    int command = 0;  // 1-based command within block, when present
    bool valid = false;
};

struct AstScript {
    std::string path;
    std::vector<Block> blocks;
    std::unordered_map<int, TextUnit> texts;
    std::unordered_map<std::string, LabelTarget> labels;
};

struct CharacterSprite {
    std::string ch;
    std::string body;
    std::string face;
    std::vector<std::string> ex;
    std::vector<std::pair<int, int>> ex_positions;
    int x = 0;
    int y = 0;
    int face_x = 0;
    int face_y = 0;
    int x2 = 0;
    int y2 = 0;
    double z = 0.0;
    int mode = 3;
    std::string size;
    double scale = 1.0;
};

enum class AudioKind {
    Bgm,
    BgmStop,
    Se,
    SeStop,
    Voice,
    VoiceStop,
};

struct AudioCommand {
    AudioKind kind = AudioKind::Se;
    std::string path;
    std::string next_path;
    bool loop = false;
    int channel = 0;
    bool transient = true;
};

struct BgmState {
    std::string path;
    std::string next_path;
    std::string logical_name;
    bool loop = false;
    bool valid = false;
};

struct ColorTone {
    bool active = false;
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

struct FlashState {
    bool active = false;
    int r = 255;
    int g = 255;
    int b = 255;
    double alpha = 0.0;
};

struct SelectOption {
    std::string text;
    std::string file;
    std::string label;
};

struct SelectState {
    bool active = false;
    int cursor = 0;
    std::vector<SelectOption> options;
};

struct GameSnapshot {
    std::string script_name;
    int block_index = 0;
    int block_line = 0;
    int command_index = 0;

    std::string background;
    std::vector<std::string> cg_layers;
    std::vector<CharacterSprite> active_fgs;
    BgmState current_bgm;
    std::string speaker;
    std::string text;
    bool textbox_visible = true;
    std::string title;

    std::map<std::string, nlohmann::json> local_vars;   // f.*
    std::map<std::string, nlohmann::json> global_vars;  // g.*
    ColorTone color_tone;
    FlashState flash;
    SelectState select;

    bool scene_ended = false;
    bool staffroll = false;
    bool brandlogo = false;
    bool title_requested = false;
    bool finished = false;
};

}  // namespace artc
