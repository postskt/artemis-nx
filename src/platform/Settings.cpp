#include "Settings.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace artc {

UserSettings load_settings(const std::filesystem::path& path) {
    UserSettings settings;
    std::ifstream input(path, std::ios::binary);
    if (!input) return settings;
    try {
        nlohmann::json value;
        input >> value;
        if (!value.is_object()) return settings;
        settings.text_speed = value.value("text_speed", settings.text_speed);
        settings.auto_speed = value.value("auto_speed", settings.auto_speed);
        settings.master_volume = value.value("master_volume", settings.master_volume);
        settings.bgm_volume = value.value("bgm_volume", settings.bgm_volume);
        settings.se_volume = value.value("se_volume", settings.se_volume);
        settings.voice_volume = value.value("voice_volume", settings.voice_volume);
        settings.sysse_volume = value.value("sysse_volume", settings.sysse_volume);
        settings.window_alpha = value.value("window_alpha", settings.window_alpha);
        settings.fullscreen = value.value("fullscreen", settings.fullscreen);
        settings.effect = value.value("effect", settings.effect);
        settings.text_color = value.value("text_color", settings.text_color);
        settings.skip_unread = value.value("skip_unread", settings.skip_unread);
        settings.bgm_voice_fade = value.value("bgm_voice_fade", settings.bgm_voice_fade);
        settings.voice_skip = value.value("voice_skip", settings.voice_skip);
        settings.auto_stop = value.value("auto_stop", settings.auto_stop);
    } catch (...) {
    }
    return settings;
}

bool save_settings(const std::filesystem::path& path, const UserSettings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    nlohmann::json value = {
        {"text_speed", settings.text_speed},
        {"auto_speed", settings.auto_speed},
        {"master_volume", settings.master_volume},
        {"bgm_volume", settings.bgm_volume},
        {"se_volume", settings.se_volume},
        {"voice_volume", settings.voice_volume},
        {"sysse_volume", settings.sysse_volume},
        {"window_alpha", settings.window_alpha},
        {"fullscreen", settings.fullscreen},
        {"effect", settings.effect},
        {"text_color", settings.text_color},
        {"skip_unread", settings.skip_unread},
        {"bgm_voice_fade", settings.bgm_voice_fade},
        {"voice_skip", settings.voice_skip},
        {"auto_stop", settings.auto_stop},
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << value.dump(2) << "\n";
    return output.good();
}

}  // namespace artc
