#include "Engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace artc {

namespace {

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string strip_ast(const std::string& name) {
    std::string value = name;
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto slash = value.find_last_of('/');
    if (slash != std::string::npos) value = value.substr(slash + 1);
    if (value.size() >= 4 && value.substr(value.size() - 4) == ".ast") {
        value = value.substr(0, value.size() - 4);
    }
    return value;
}

int parse_color(const std::string& value) {
    try {
        if (starts_with(value, "0x") || starts_with(value, "0X")) {
            return static_cast<int>(std::stoul(value.substr(2), nullptr, 16));
        }
        return std::stoi(value);
    } catch (...) {
        return 0;
    }
}

bool truthy(const nlohmann::json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return std::fabs(value.get<double>()) > 1e-12;
    if (value.is_string()) return !value.get<std::string>().empty();
    return true;
}

double numeric(const nlohmann::json& value, bool& ok) {
    ok = value.is_number();
    return ok ? value.get<double>() : 0.0;
}

}  // namespace

Engine::Engine(const Config& config, AssetManager& assets, LuaLoader& loader)
    : config_(config), assets_(assets), loader_(loader) {
    snapshot_.textbox_visible = true;
}

void Engine::start() {
    trace("start entry");
    scripts_.clear();
    local_vars_.clear();
    global_vars_.clear();
    audio_queue_.clear();
    active_fgs_.clear();
    cg_layers_.clear();
    current_bgm_ = {};
    select_ = {};
    finished_ = false;
    waiting_text_ = false;
    jumped_ = false;
    block_index_ = 1;
    command_index_ = 0;
    snapshot_ = GameSnapshot{};
    snapshot_.textbox_visible = true;

    if (!load_script(config_.entry_script())) {
        warn("cannot load entry script: " + config_.entry_script());
        finished_ = true;
        update_snapshot();
        return;
    }
    trace("entry loaded");
    block_index_ = 1;
    command_index_ = 0;
    run_until_wait();
    trace("start done block=" + std::to_string(block_index_) +
          " finished=" + std::to_string(finished_) +
          " text=" + std::to_string(snapshot_.text.size()));
    update_snapshot();
}

void Engine::step(Action action) {
    if (finished_) return;
    if (action == Action::Quit) {
        finished_ = true;
        waiting_text_ = false;
        select_.active = false;
        update_snapshot();
        return;
    }
    if (action == Action::ToggleUi) {
        snapshot_.textbox_visible = !snapshot_.textbox_visible;
        return;
    }
    if (action == Action::ToggleDebug) {
        return;  // presentation-only OSD
    }
    if (select_.active) {
        if ((action == Action::Up || action == Action::SelectUp) && !select_.options.empty()) {
            select_.cursor = (select_.cursor + static_cast<int>(select_.options.size()) - 1) %
                             static_cast<int>(select_.options.size());
        } else if ((action == Action::Down || action == Action::SelectDown) && !select_.options.empty()) {
            select_.cursor = (select_.cursor + 1) % static_cast<int>(select_.options.size());
        } else if (action == Action::Confirm || action == Action::Next) {
            if (!select_.options.empty()) {
                SelectOption selected = select_.options[select_.cursor];
                select_ = {};
                if (!selected.file.empty()) {
                    const std::string name = strip_ast(selected.file) + ".ast";
                    if (!load_script(name)) {
                        warn("select target script missing: " + selected.file);
                    } else {
                        block_index_ = 1;
                        command_index_ = 0;
                        jumped_ = true;
                    }
                }
                if (!selected.label.empty() && script_) {
                    auto it = script_->labels.find(selected.label);
                    if (it != script_->labels.end() && it->second.valid) {
                        block_index_ = it->second.block;
                        command_index_ = std::max(0, it->second.command - 1);
                        jumped_ = true;
                    } else {
                        warn("select label missing: " + selected.label);
                    }
                }
                run_until_wait();
            }
        } else if (action == Action::Cancel) {
            select_ = {};
            run_until_wait();
        }
        update_snapshot();
        return;
    }
    if (action == Action::ToggleUi) {
        snapshot_.textbox_visible = !snapshot_.textbox_visible;
        return;
    }
    if (waiting_text_) {
        if (action == Action::Next || action == Action::Skip || action == Action::Confirm) {
            waiting_text_ = false;
            advance_block();
            run_until_wait();
            update_snapshot();
        }
        return;
    }
    if (action == Action::Next || action == Action::Skip || action == Action::Confirm) {
        run_until_wait();
        update_snapshot();
    }
}

std::vector<AudioCommand> Engine::consume_audio() {
    std::vector<AudioCommand> result(audio_queue_.begin(), audio_queue_.end());
    audio_queue_.clear();
    return result;
}

bool Engine::load_script(const std::string& name) {
    const std::string key = strip_ast(name) + ".ast";
    trace("load_script " + key);
    auto cached = scripts_.find(key);
    if (cached != scripts_.end()) {
        script_ = &cached->second;
        script_name_ = key;
        return true;
    }
    const std::string relative = config_.script_path(key);
    auto actual = assets_.resolve(relative);
    if (!actual) return false;
    try {
        AstScript loaded = loader_.load_ast(assets_.root() / *actual);
        auto inserted = scripts_.emplace(key, std::move(loaded));
        script_ = &inserted.first->second;
        script_name_ = key;
        trace("loaded " + key + " blocks=" + std::to_string(script_->blocks.size()));
        return true;
    } catch (const std::exception& e) {
        warn("AST load failed: " + key + ": " + e.what());
        return false;
    }
}

void Engine::goto_script(const std::string& name, int block, int command) {
    const std::string key = strip_ast(name) + ".ast";
    if (load_script(key)) {
        block_index_ = std::max(1, block);
        command_index_ = std::max(0, command);
        jumped_ = true;
    }
}

void Engine::run_until_wait() {
    loop_guard_ = 0;
    while (!finished_ && !waiting_text_ && !select_.active) {
        if (!script_ || block_index_ < 1 ||
            block_index_ > static_cast<int>(script_->blocks.size())) {
            finished_ = true;
            break;
        }
        if (++loop_guard_ > 100000) {
            warn("engine loop guard exceeded");
            finished_ = true;
            break;
        }
        Block& block = script_->blocks[block_index_ - 1];
        trace("block " + script_name_ + ":" + std::to_string(block_index_) +
              " commands=" + std::to_string(block.commands.size()));
        execute_block(block);
        if (jumped_) {
            jumped_ = false;
            continue;
        }
        if (waiting_text_ || select_.active) break;
        advance_block();
    }
}

void Engine::advance_block() {
    if (!script_) return;
    if (block_index_ < static_cast<int>(script_->blocks.size())) {
        ++block_index_;
    } else {
        finished_ = true;
    }
    command_index_ = 0;
}

void Engine::execute_block(Block& block) {
    while (command_index_ < static_cast<int>(block.commands.size())) {
        const Command& command = block.commands[command_index_++];
        const bool waiting = execute_command(command);
        if (waiting || waiting_text_ || select_.active || jumped_) break;
    }
}

bool Engine::execute_command(const Command& command) {
    const std::string& name = command.name;
    trace("command " + name);
    if (name == "text") {
        if (!script_) {
            waiting_text_ = true;
            return true;
        }
        auto it = script_->texts.find(block_index_);
        if (it == script_->texts.end()) {
            warn("missing text for block " + std::to_string(block_index_));
            snapshot_.speaker.clear();
            snapshot_.text.clear();
            audio_queue_.push_back(AudioCommand{AudioKind::VoiceStop});
            waiting_text_ = true;
            return true;
        }
        show_text(it->second);
        return true;
    }
    if (name == "bg") {
        cmd_bg(command);
    } else if (name == "fg") {
        cmd_fg(command);
    } else if (name == "cgdel") {
        cmd_cgdel(command);
    } else if (name == "fgact") {
        // Expression animation is presentation-only.
    } else if (name == "bgm") {
        cmd_bgm(command);
    } else if (name == "se") {
        cmd_se(command);
    } else if (name == "vo") {
        cmd_vo(command);
    } else if (name == "vostop") {
        audio_queue_.push_back(AudioCommand{AudioKind::VoiceStop});
    } else if (name == "msgoff") {
        snapshot_.textbox_visible = false;
    } else if (name == "msgon") {
        snapshot_.textbox_visible = true;
    } else if (name == "savetitle") {
        snapshot_.title = command.str("text");
    } else if (name == "excall") {
        cmd_excall(command);
    } else if (name == "eval") {
        cmd_eval(command);
    } else if (name == "select") {
        begin_select(command);
    } else if (name == "colortone") {
        cmd_colortone(command);
    } else if (name == "flash") {
        cmd_flash(command);
    } else if (name == "sceneend") {
        snapshot_.scene_ended = true;
    } else if (name == "staffroll") {
        snapshot_.staffroll = true;
    } else if (name == "brandlogo") {
        snapshot_.brandlogo = true;
    } else if (name == "gotitle" || name == "タイトル") {
        snapshot_.title_requested = true;
        reset_to_entry();
    } else if (name == "extrans" || name == "ex" || name == "ntclear" ||
               name == "cacheclear" || name == "quake" || name == "wait" ||
               name == "trace" || name == "debug") {
        // Presentation/engine-only tags tolerated by the MVP core.
    } else {
        warn("unknown command ignored: " + name);
    }
    return false;
}

void Engine::show_text(const TextUnit& unit) {
    audio_queue_.push_back(AudioCommand{AudioKind::VoiceStop});
    snapshot_.speaker = unit.speaker.value_or("");
    snapshot_.text = unit.text;
    trace("show_text speaker=" + snapshot_.speaker +
          " text=" + std::to_string(snapshot_.text.size()));
    snapshot_.textbox_visible = true;
    waiting_text_ = true;
    for (const auto& voice : unit.voices) {
        auto path = resolve_voice(voice.file, voice.ch);
        if (!path) continue;
        AudioCommand command;
        command.kind = AudioKind::Voice;
        command.path = *path;
        command.channel = config_.audio().voice_channels.empty() ? 0 : config_.audio().voice_channels.front();
        audio_queue_.push_back(std::move(command));
    }
}

void Engine::cmd_bg(const Command& command) {
    const std::string file = command.str("file");
    if (file.empty()) return;
    std::string prefix = command.str("path", ":bg/");
    trace("bg file=" + file + " prefix=" + prefix);
    auto path = resolve_magic_file(prefix, file, config_.image_ext());
    if (!path) return;
    trace("bg resolved=" + *path);
    if (prefix == ":ev/") {
        // Event CGs are full-screen scene replacements, not overlays like
        // :cg/ cinema frames.  The original scene's standing sprites must not
        // remain on top of the event art.
        active_fgs_.clear();
        cg_layers_.clear();
    }
    if (prefix == ":cg/") {
        const int id = command.integer("id", command.integer("lv", 0));
        cg_layers_[std::to_string(id)] = *path;
    } else {
        snapshot_.background = *path;
    }
}

void Engine::cmd_cgdel(const Command& command) {
    const int id = command.integer("id", -1);
    if (id == -1) {
        cg_layers_.clear();
        return;
    }
    cg_layers_.erase(std::to_string(id));
}

void Engine::cmd_fg(const Command& command) {
    const int mode = command.integer("mode", 3);
    std::string ch = command.str("ch");
    if (ch.empty()) ch = std::to_string(command.integer("id", command.integer("file", 0)));
    if (mode == -2) {
        active_fgs_.clear();
        return;
    }
    if (mode == -1) {
        active_fgs_.erase(std::remove_if(active_fgs_.begin(), active_fgs_.end(),
                                         [&](const CharacterSprite& s) { return s.ch == ch; }),
                          active_fgs_.end());
        return;
    }
    if (mode < 2) return;

    const std::string file = command.str("file");
    const std::string path_prefix = command.str("path", ":fg/");
    if (file.empty()) return;
    auto body = resolve_magic_file(path_prefix, file, config_.image_ext());
    if (!body) return;
    const std::string head = command.str("head");
    const std::string face = command.str("face");
    const auto base = config_.fg_position(head, file);
    const int base_x = base ? base->x : 0;
    const int base_y = base ? base->y : 0;

    CharacterSprite sprite;
    sprite.ch = ch;
    sprite.body = *body;
    sprite.x = base_x + command.integer("x", 0) + command.integer("mx", 0);
    sprite.y = base_y + command.integer("y", 0) + command.integer("my", 0);
    sprite.x2 = command.integer("x2", 0);
    sprite.y2 = command.integer("y2", 0);
    sprite.z = command.number("lv", 0.0);
    sprite.mode = mode;
    sprite.size = command.str("size");
    sprite.scale = config_.fg_scale(sprite.size);

    if (!face.empty()) {
        auto face_path = resolve_magic_file(path_prefix, face, config_.image_ext());
        if (face_path) {
            sprite.face = *face_path;
            const auto fp = config_.fg_position(head, face);
            if (fp) {
                sprite.face_x = fp->x - base_x;
                sprite.face_y = fp->y - base_y;
            }
        }
    }
    for (int i = 1; i <= 4; ++i) {
        const std::string key = "ex0" + std::to_string(i);
        const std::string ex_name = command.str(key);
        if (ex_name.empty()) continue;
        auto ex_path = resolve_magic_file(path_prefix, ex_name, config_.image_ext());
        if (!ex_path) continue;
        sprite.ex.push_back(*ex_path);
        const auto ep = config_.fg_position(head, ex_name);
        sprite.ex_positions.emplace_back(ep ? ep->x - base_x : 0,
                                        ep ? ep->y - base_y : 0);
    }

    auto it = std::find_if(active_fgs_.begin(), active_fgs_.end(),
                           [&](const CharacterSprite& s) { return s.ch == ch; });
    if (it == active_fgs_.end()) {
        active_fgs_.push_back(std::move(sprite));
    } else {
        *it = std::move(sprite);
    }
    std::sort(active_fgs_.begin(), active_fgs_.end(),
              [](const CharacterSprite& a, const CharacterSprite& b) {
                  if (a.z != b.z) return a.z < b.z;
                  return a.ch < b.ch;
              });
}

void Engine::cmd_bgm(const Command& command) {
    if (command.integer("stop", 0) == 1) {
        audio_queue_.push_back(AudioCommand{AudioKind::BgmStop});
        current_bgm_ = {};
        return;
    }
    const std::string name = command.str("file");
    if (name.empty()) return;
    const BgmEntry* entry = config_.bgm(name);
    std::string intro = entry ? entry->intro : name + config_.audio().sound_ext;
    std::string loop = entry ? entry->loop : std::string();
    if (entry && !entry->intro_exists) {
        warn("BGM has no intro asset: " + name + " -> " + intro);
        return;
    }
    auto intro_path = resolve_asset(config_.resolve_magic(":bgm/") + intro);
    if (!intro_path) return;
    std::string loop_path;
    if (!loop.empty()) {
        auto path = resolve_asset(config_.resolve_magic(":bgm/") + loop);
        if (path) loop_path = *path;
    }
    current_bgm_ = BgmState{*intro_path, loop_path, name,
                            !loop_path.empty() || command.integer("loop", 1) != 0, true};
    AudioCommand audio;
    audio.kind = AudioKind::Bgm;
    audio.path = *intro_path;
    audio.next_path = loop_path;
    audio.loop = true;
    audio_queue_.push_back(std::move(audio));
}

void Engine::cmd_se(const Command& command) {
    if (command.integer("stop", 0) == 1) {
        AudioCommand audio;
        audio.kind = AudioKind::SeStop;
        audio.channel = command.integer("id", 0);
        audio_queue_.push_back(std::move(audio));
        return;
    }
    const std::string file = command.str("file");
    if (file.empty()) return;
    auto path = resolve_se_file(file);
    if (!path) return;
    AudioCommand audio;
    audio.kind = AudioKind::Se;
    audio.path = *path;
    audio.channel = command.integer("id", 0);
    audio_queue_.push_back(std::move(audio));
}

void Engine::cmd_vo(const Command& command) {
    const std::string file = command.str("file");
    const std::string ch = command.str("ch");
    if (file.empty() || ch.empty()) return;
    auto path = resolve_voice(file, ch);
    if (!path) return;
    AudioCommand audio;
    audio.kind = AudioKind::Voice;
    audio.path = *path;
    audio.channel = config_.audio().voice_channels.empty() ? 0 : config_.audio().voice_channels.front();
    audio_queue_.push_back(std::move(audio));
}

void Engine::cmd_colortone(const Command& command) {
    const std::string mode = command.str("mode");
    if (mode == "reset" || mode == "off" || mode.empty()) {
        snapshot_.color_tone = {};
        return;
    }
    auto it = config_.color_tones().find(mode);
    if (it == config_.color_tones().end()) {
        warn("unknown colortone mode: " + mode);
        return;
    }
    const int color = parse_color(it->second);
    snapshot_.color_tone.active = true;
    snapshot_.color_tone.r = (color >> 16) & 0xFF;
    snapshot_.color_tone.g = (color >> 8) & 0xFF;
    snapshot_.color_tone.b = color & 0xFF;
    snapshot_.color_tone.a = 255;
}

void Engine::cmd_flash(const Command& command) {
    snapshot_.flash.active = true;
    snapshot_.flash.r = 255;
    snapshot_.flash.g = 255;
    snapshot_.flash.b = 255;
    const double alpha = command.number("alpha", 1.0);
    snapshot_.flash.alpha = alpha > 1.0 ? alpha / 255.0 : alpha;
}

void Engine::cmd_excall(const Command& command) {
    if (!command.str("cond").empty() && !evaluate_condition(command.str("cond"))) return;
    const std::string file = command.str("file");
    const std::string label = command.str("label");
    trace("excall file=" + file + " label=" + label);
    if (!file.empty()) {
        const std::string target = strip_ast(file) + ".ast";
        if (!load_script(target)) {
            warn("excall target missing: " + file);
            return;
        }
        block_index_ = 1;
        command_index_ = 0;
        jumped_ = true;
    }
    if (!label.empty() && script_) {
        auto it = script_->labels.find(label);
        if (it == script_->labels.end() || !it->second.valid) {
            warn("excall label missing: " + label);
            return;
        }
        block_index_ = it->second.block;
        command_index_ = std::max(0, it->second.command - 1);
        jumped_ = true;
    }
}

void Engine::cmd_eval(const Command& command) {
    const std::string exp = command.str("exp");
    if (!exp.empty()) assign_variable(exp);
}

bool Engine::begin_select(const Command& command) {
    if (!script_ || block_index_ < 1 ||
        block_index_ > static_cast<int>(script_->blocks.size())) return false;
    const Block& block = script_->blocks[block_index_ - 1];
    std::vector<SelectOption> options;
    int index = command_index_ - 1;
    if (index < 0 || index >= static_cast<int>(block.commands.size())) return false;
    const Command& first = block.commands[index];
    if (first.name != "select") return false;

    for (; index < static_cast<int>(block.commands.size()); ++index) {
        const Command& current = block.commands[index];
        if (current.name != "select") break;
        if (current.str("mode") == "hide") {
            ++index;
            break;
        }
        SelectOption option;
        option.text = current.str("text");
        option.file = current.str("file");
        option.label = current.str("label");
        if (!option.text.empty() || !option.file.empty() || !option.label.empty()) {
            options.push_back(std::move(option));
        }
    }
    if (options.empty()) return false;
    command_index_ = index;
    select_.active = true;
    select_.cursor = 0;
    select_.options = std::move(options);
    return true;
}

std::optional<std::string> Engine::resolve_asset(const std::string& relative) const {
    return assets_.resolve(relative);
}

std::optional<std::string> Engine::resolve_magic_file(const std::string& magic_prefix,
                                                      const std::string& file,
                                                      const std::string& extension) const {
    const std::string base = config_.resolve_magic(magic_prefix);
    if (base == magic_prefix) {
        warn("unknown magic path: " + magic_prefix);
        return std::nullopt;
    }
    std::string name = file;
    if (name.size() < extension.size() ||
        name.compare(name.size() - extension.size(), extension.size(), extension) != 0) {
        name += extension;
    }
    return assets_.resolve(config_.join(base, name));
}

std::optional<std::string> Engine::resolve_voice(const std::string& file,
                                                 const std::string& channel) const {
    return resolve_magic_file(":vo/", channel + "/" + file, config_.audio().sound_ext);
}

std::optional<std::string> Engine::resolve_bgm_file(const std::string& file_name) const {
    return resolve_magic_file(":bgm/", file_name, config_.audio().sound_ext);
}

std::optional<std::string> Engine::resolve_se_file(const std::string& file_name) const {
    return resolve_magic_file(":se/", file_name, config_.audio().sound_ext);
}

nlohmann::json Engine::variable_value(const std::string& expression) const {
    const std::string expr = trim(expression);
    if (expr.empty()) return nullptr;
    if (expr == "true") return true;
    if (expr == "false") return false;
    if ((expr.front() == '"' && expr.back() == '"') ||
        (expr.front() == '\'' && expr.back() == '\'')) {
        return expr.substr(1, expr.size() - 2);
    }
    if (starts_with(expr, "g.")) {
        auto it = global_vars_.find(expr.substr(2));
        return it == global_vars_.end() ? nlohmann::json(nullptr) : it->second;
    }
    if (starts_with(expr, "f.")) {
        auto it = local_vars_.find(expr.substr(2));
        return it == local_vars_.end() ? nlohmann::json(nullptr) : it->second;
    }
    try {
        if (expr.find('.') != std::string::npos) return std::stod(expr);
        return std::stoll(expr);
    } catch (...) {
        return expr;
    }
}

bool Engine::evaluate_condition(const std::string& expression) const {
    const std::string expr = trim(expression);
    const std::vector<std::pair<std::string, int>> operators = {
        {"==", 0}, {"!=", 0}, {">=", 0}, {"<=", 0}, {">", 0}, {"<", 0},
    };
    for (const auto& [op, _] : operators) {
        const auto pos = expr.find(op);
        if (pos == std::string::npos) continue;
        const auto lhs = variable_value(expr.substr(0, pos));
        const auto rhs = variable_value(expr.substr(pos + op.size()));
        if (op == "==") return lhs == rhs;
        if (op == "!=") return lhs != rhs;
        bool ln = false, rn = false;
        const double l = numeric(lhs, ln);
        const double r = numeric(rhs, rn);
        if (!ln || !rn) return false;
        if (op == ">") return l > r;
        if (op == "<") return l < r;
        if (op == ">=") return l >= r;
        if (op == "<=") return l <= r;
    }
    return truthy(variable_value(expr));
}

void Engine::assign_variable(const std::string& expression) {
    const auto pos = expression.find('=');
    if (pos == std::string::npos) return;
    if (pos + 1 < expression.size() && expression[pos + 1] == '=') return;
    const std::string lhs = trim(expression.substr(0, pos));
    const std::string rhs = trim(expression.substr(pos + 1));
    if (lhs.empty()) return;
    const auto dot = lhs.find('.');
    if (dot == std::string::npos) return;
    set_variable(lhs.substr(0, dot), lhs.substr(dot + 1), variable_value(rhs));
}

void Engine::set_variable(const std::string& scope, const std::string& name,
                          const nlohmann::json& value) {
    if (scope == "g") {
        global_vars_[name] = value;
    } else if (scope == "f") {
        local_vars_[name] = value;
    }
}

void Engine::reset_to_entry() {
    audio_queue_.push_back(AudioCommand{AudioKind::VoiceStop});
    audio_queue_.push_back(AudioCommand{AudioKind::BgmStop});
    active_fgs_.clear();
    cg_layers_.clear();
    current_bgm_ = {};
    select_ = {};
    waiting_text_ = false;
    snapshot_.speaker.clear();
    snapshot_.text.clear();
    snapshot_.background.clear();
    snapshot_.flash = {};
    snapshot_.color_tone = {};
    local_vars_.clear();
    if (!load_script(config_.entry_script())) {
        finished_ = true;
        return;
    }
    block_index_ = 1;
    command_index_ = 0;
    jumped_ = true;
    snapshot_.title_requested = true;
}

void Engine::update_snapshot() {
    snapshot_.script_name = script_ ? script_name_ : "";
    snapshot_.block_index = block_index_;
    snapshot_.block_line = (script_ && block_index_ >= 1 &&
                            block_index_ <= static_cast<int>(script_->blocks.size()))
                               ? script_->blocks[block_index_ - 1].line
                               : 0;
    snapshot_.command_index = command_index_;
    snapshot_.active_fgs = active_fgs_;
    snapshot_.cg_layers.clear();
    for (const auto& [key, path] : cg_layers_) snapshot_.cg_layers.push_back(path);
    snapshot_.current_bgm = current_bgm_;
    snapshot_.local_vars = local_vars_;
    snapshot_.global_vars = global_vars_;
    snapshot_.select = select_;
    snapshot_.finished = finished_;
}

void Engine::warn(const std::string& message) const {
    assets_.warn(message);
}

void Engine::trace(const std::string& message) const {
    if (trace_sink_) trace_sink_(message);
}

}  // namespace artc
