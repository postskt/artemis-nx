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
        settings.bgm_volume = value.value("bgm_volume", settings.bgm_volume);
        settings.se_volume = value.value("se_volume", settings.se_volume);
        settings.voice_volume = value.value("voice_volume", settings.voice_volume);
        settings.fullscreen = value.value("fullscreen", settings.fullscreen);
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
        {"bgm_volume", settings.bgm_volume},
        {"se_volume", settings.se_volume},
        {"voice_volume", settings.voice_volume},
        {"fullscreen", settings.fullscreen},
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << value.dump(2) << "\n";
    return output.good();
}

}  // namespace artc
