#pragma once

#include <list>
#include <memory>
#include <string>
#include <unordered_map>

#include <SDL.h>
#include <SDL_ttf.h>

#include "AssetManager.hpp"
#include "Config.hpp"
#include "Engine.hpp"

namespace artc {

class TextureCache {
public:
    explicit TextureCache(SDL_Renderer* renderer, std::size_t capacity = 64);
    ~TextureCache();

    SDL_Texture* get(const std::filesystem::path& path);
    void clear();
    std::size_t size() const { return entries_.size(); }
    std::size_t capacity() const { return capacity_; }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }

private:
    struct Entry {
        SDL_Texture* texture = nullptr;
        std::list<std::string>::iterator lru;
    };
    SDL_Renderer* renderer_;
    std::size_t capacity_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

class Renderer {
public:
    Renderer(SDL_Window* window, const Config& config, const AssetManager& assets);
    ~Renderer();

    void draw(const GameSnapshot& snapshot);
    void present();
    bool save_png(const std::string& path);
    void toggle_debug() { debug_ = !debug_; }
    bool debug() const { return debug_; }
    TextureCache& textures() { return *textures_; }

private:
    const Config& config_;
    const AssetManager& assets_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<TextureCache> textures_;
    TTF_Font* body_font_ = nullptr;
    TTF_Font* name_font_ = nullptr;
    TTF_Font* select_font_ = nullptr;
    TTF_Font* osd_font_ = nullptr;
    bool debug_ = false;

    int body_size_ = 38;
    int body_left_ = 115;
    int body_top_ = 605;
    int body_width_ = 750;
    int name_size_ = 36;
    int name_left_ = 264;
    int name_top_ = 518;
    SDL_Color body_color_{255, 255, 255, 255};
    SDL_Color body_outline_{52, 24, 32, 255};
    SDL_Color name_color_{207, 96, 134, 255};
    SDL_Color name_outline_{255, 255, 255, 255};

    void load_layout();
    void draw_background(const GameSnapshot& snapshot);
    void draw_characters(const GameSnapshot& snapshot);
    void draw_filters(const GameSnapshot& snapshot);
    void draw_textbox(const GameSnapshot& snapshot);
    void draw_select(const GameSnapshot& snapshot);
    void draw_osd(const GameSnapshot& snapshot);

    void draw_text(const std::string& text, TTF_Font* font, SDL_Color color,
                   SDL_Color outline, int x, int y, int wrap_width,
                   int line_height);
    static std::vector<std::string> wrap_text(TTF_Font* font, const std::string& text,
                                              int width);
    static SDL_Color parse_color(const std::string& value, SDL_Color fallback);
    static std::string path_of(const AssetManager& assets, const std::string& relative);
};

}  // namespace artc
