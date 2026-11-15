#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AssetManager.hpp"
#include "AstModel.hpp"
#include "Config.hpp"
#include "LuaLoader.hpp"

namespace artc {

enum class Action {
    Next,
    Skip,
    ToggleUi,
    Up,
    Down,
    SelectUp,
    SelectDown,
    Left,
    Right,
    PageLeft,
    PageRight,
    Pointer,
    Back,
    ToggleDebug,
    Menu,
    QuickSave,
    QuickLoad,
    Confirm,
    Cancel,
    Quit,
};

class Engine {
public:
    Engine(const Config& config, AssetManager& assets, LuaLoader& loader);

    void start();
    void step(Action action = Action::Next);
    void set_trace(std::function<void(const std::string&)> sink) { trace_sink_ = std::move(sink); }
    const GameSnapshot& snapshot() const { return snapshot_; }
    std::vector<AudioCommand> consume_audio();
    bool finished() const { return finished_; }

    nlohmann::json export_state() const;
    bool import_state(const nlohmann::json& state, std::string* error = nullptr);

    const std::string& current_script_name() const { return script_name_; }

private:
    const Config& config_;
    AssetManager& assets_;
    LuaLoader& loader_;

    std::map<std::string, AstScript> scripts_;
    std::string script_name_;
    AstScript* script_ = nullptr;

    int block_index_ = 1;
    int command_index_ = 0;
    bool waiting_text_ = false;
    bool finished_ = false;
    bool jumped_ = false;
    int loop_guard_ = 0;

    GameSnapshot snapshot_;
    std::map<std::string, nlohmann::json> local_vars_;
    std::map<std::string, nlohmann::json> global_vars_;
    std::deque<AudioCommand> audio_queue_;
    std::vector<CharacterSprite> active_fgs_;
    std::map<std::string, std::string> cg_layers_;
    BgmState current_bgm_;
    SelectState select_;

    bool load_script(const std::string& name);
    void goto_script(const std::string& name, int block, int command);
    void run_until_wait();
    void execute_block(Block& block);
    bool execute_command(const Command& command);

    void advance_block();
    void clear_text();
    void show_text(const TextUnit& unit);
    void update_snapshot();

    void cmd_bg(const Command& command);
    void cmd_fg(const Command& command);
    void cmd_cgdel(const Command& command);
    void cmd_bgm(const Command& command);
    void cmd_se(const Command& command);
    void cmd_vo(const Command& command);
    void cmd_colortone(const Command& command);
    void cmd_flash(const Command& command);
    void cmd_excall(const Command& command);
    void cmd_eval(const Command& command);
    bool begin_select(const Command& command);

    std::optional<std::string> resolve_asset(const std::string& relative) const;
    std::optional<std::string> resolve_magic_file(const std::string& magic_prefix,
                                                  const std::string& file,
                                                  const std::string& extension) const;
    std::optional<std::string> resolve_voice(const std::string& file,
                                             const std::string& channel) const;
    std::optional<std::string> resolve_bgm_file(const std::string& file_name) const;
    std::optional<std::string> resolve_se_file(const std::string& file_name) const;

    nlohmann::json variable_value(const std::string& expression) const;
    bool evaluate_condition(const std::string& expression) const;
    void assign_variable(const std::string& expression);
    void set_variable(const std::string& scope, const std::string& name,
                      const nlohmann::json& value);

    void reset_to_entry();
    void warn(const std::string& message) const;
    void trace(const std::string& message) const;
    std::function<void(const std::string&)> trace_sink_;
};

}  // namespace artc
