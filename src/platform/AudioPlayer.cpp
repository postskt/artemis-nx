#include "AudioPlayer.hpp"

#include <cstdio>
#include <utility>

namespace artc {

AudioPlayer* AudioPlayer::active_ = nullptr;

AudioPlayer::AudioPlayer(const Config& config, const AssetManager& assets)
    : config_(config), assets_(assets) {
    active_ = this;
    const int flags = Mix_Init(MIX_INIT_OGG);
    if (!(flags & MIX_INIT_OGG)) {
        warn(std::string("SDL_mixer OGG support unavailable: ") + Mix_GetError());
    }
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
        warn(std::string("Mix_OpenAudio failed: ") + Mix_GetError());
        return;
    }
    initialized_ = true;
    Mix_AllocateChannels(8);
    Mix_ReserveChannels(1);  // channel 0 is reserved for voice
    Mix_HookMusicFinished(&AudioPlayer::music_finished_callback);
    available_ = true;
}

AudioPlayer::~AudioPlayer() {
    shutdown();
    if (active_ == this) active_ = nullptr;
}

void AudioPlayer::set_volumes(float bgm, float se, float voice) {
    bgm_volume_ = bgm < 0.0f ? 0.0f : (bgm > 1.0f ? 1.0f : bgm);
    se_volume_ = se < 0.0f ? 0.0f : (se > 1.0f ? 1.0f : se);
    voice_volume_ = voice < 0.0f ? 0.0f : (voice > 1.0f ? 1.0f : voice);
    if (!available_) return;
    Mix_VolumeMusic(static_cast<int>(bgm_volume_ * MIX_MAX_VOLUME));
    Mix_Volume(0, static_cast<int>(voice_volume_ * MIX_MAX_VOLUME));
    for (int channel = 1; channel < 8; ++channel) {
        Mix_Volume(channel, static_cast<int>(se_volume_ * MIX_MAX_VOLUME));
    }
}

void AudioPlayer::shutdown() {    if (!initialized_) return;
    Mix_HaltChannel(-1);
    stop_music();
    Mix_HookMusicFinished(nullptr);
    for (auto& [path, chunk] : chunks_) {
        (void)path;
        if (chunk) Mix_FreeChunk(chunk);
    }
    chunks_.clear();
    Mix_CloseAudio();
    Mix_Quit();
    initialized_ = false;
    available_ = false;
}

void AudioPlayer::play(const std::vector<AudioCommand>& commands) {
    if (!available_) return;
    for (const auto& command : commands) {
        switch (command.kind) {
            case AudioKind::VoiceStop:
                Mix_HaltChannel(0);
                break;
            case AudioKind::Voice: {
                Mix_Chunk* chunk = load_chunk(command.path);
                if (chunk) {
                    Mix_HaltChannel(0);
                    if (Mix_PlayChannel(0, chunk, 0) < 0) {
                        warn(std::string("voice play failed: ") + Mix_GetError());
                    }
                }
                break;
            }
            case AudioKind::Se: {
                Mix_Chunk* chunk = load_chunk(command.path);
                if (chunk) {
                    const int channel = 1 + ((command.channel % 7) + 7) % 7;
                    if (Mix_PlayChannel(channel, chunk, 0) < 0) {
                        const int fallback = 1 + (se_cursor_++ % 7);
                        Mix_PlayChannel(fallback, chunk, 0);
                    }
                }
                break;
            }
            case AudioKind::SeStop: {
                const int channel = 1 + ((command.channel % 7) + 7) % 7;
                Mix_HaltChannel(channel);
                break;
            }
            case AudioKind::Bgm:
                play_bgm(command);
                break;
            case AudioKind::BgmStop:
                stop_music();
                break;
        }
    }
}

void AudioPlayer::update() {
    if (!available_) return;
    if (!music_finished_.exchange(false)) return;
    if (next_music_.empty()) return;
    const std::string next = next_music_;
    next_music_.clear();
    Mix_Music* loop = Mix_LoadMUS(asset_path(next).c_str());
    if (!loop) {
        warn(std::string("cannot load BGM loop: ") + next + ": " + Mix_GetError());
        return;
    }
    if (music_) Mix_FreeMusic(music_);
    music_ = loop;
    Mix_HookMusicFinished(nullptr);
    if (Mix_PlayMusic(music_, -1) < 0) {
        warn(std::string("cannot loop BGM: ") + Mix_GetError());
    }
}

void AudioPlayer::play_bgm(const AudioCommand& command) {
    stop_music();
    if (command.path.empty()) return;
    const std::string path = asset_path(command.path);
    music_ = Mix_LoadMUS(path.c_str());
    if (!music_) {
        warn("cannot load BGM: " + command.path + ": " + Mix_GetError());
        return;
    }
    next_music_ = command.next_path;
    music_finished_.store(false);
    Mix_HookMusicFinished(&AudioPlayer::music_finished_callback);
    const int loops = command.next_path.empty() ? -1 : 0;
    if (Mix_PlayMusic(music_, loops) < 0) {
        warn(std::string("cannot play BGM: ") + Mix_GetError());
    }
}

void AudioPlayer::stop_music() {
    if (music_) {
        Mix_HaltMusic();
        Mix_FreeMusic(music_);
        music_ = nullptr;
    }
    next_music_.clear();
    music_finished_.store(false);
}

Mix_Chunk* AudioPlayer::load_chunk(const std::string& relative) {
    if (relative.empty()) return nullptr;
    auto it = chunks_.find(relative);
    if (it != chunks_.end()) return it->second;
    Mix_Chunk* chunk = Mix_LoadWAV(asset_path(relative).c_str());
    if (!chunk) {
        warn("cannot load audio: " + relative + ": " + Mix_GetError());
        return nullptr;
    }
    chunks_[relative] = chunk;
    return chunk;
}

std::string AudioPlayer::asset_path(const std::string& relative) const {
    return (assets_.root() / relative).string();
}

void AudioPlayer::music_finished_callback() {
    if (active_) active_->music_finished_.store(true);
}

void AudioPlayer::warn(const std::string& message) {
    std::fprintf(stderr, "[audio] %s\n", message.c_str());
}

}  // namespace artc
