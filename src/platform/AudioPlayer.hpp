#pragma once

#include <atomic>
#include <string>
#include <unordered_map>

#include <SDL_mixer.h>

#include "AssetManager.hpp"
#include "AstModel.hpp"
#include "Config.hpp"

namespace artc {

class AudioPlayer {
public:
    AudioPlayer(const Config& config, const AssetManager& assets);
    ~AudioPlayer();

    void play(const std::vector<AudioCommand>& commands);
    void update();
    void shutdown();

    bool available() const { return available_; }
    int voice_channel() const { return 0; }

private:
    const Config& config_;
    const AssetManager& assets_;
    bool available_ = false;
    bool initialized_ = false;
    Mix_Music* music_ = nullptr;
    std::string next_music_;
    std::atomic<bool> music_finished_{false};
    int se_cursor_ = 1;
    std::unordered_map<std::string, Mix_Chunk*> chunks_;

    static AudioPlayer* active_;
    static void music_finished_callback();

    std::string asset_path(const std::string& relative) const;
    Mix_Chunk* load_chunk(const std::string& relative);
    void stop_music();
    void play_bgm(const AudioCommand& command);
    void warn(const std::string& message);
};

}  // namespace artc
