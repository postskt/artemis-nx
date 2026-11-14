#pragma once

#include <filesystem>

namespace artc {

struct UserSettings {
    float text_speed = 1.0f;
    float auto_speed = 1.0f;
    float bgm_volume = 1.0f;
    float se_volume = 1.0f;
    float voice_volume = 1.0f;
    bool fullscreen = false;
};

UserSettings load_settings(const std::filesystem::path& path);
bool save_settings(const std::filesystem::path& path, const UserSettings& settings);

}  // namespace artc
