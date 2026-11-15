#pragma once

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>
#include <SDL_ttf.h>

#include "AssetManager.hpp"
#include "Config.hpp"
#include "Engine.hpp"
#include "Gallery.hpp"
#include "SaveManager.hpp"
#include "Settings.hpp"

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
    void set_fade(float alpha) { fade_alpha_ = alpha; }
    void begin_press(int x, int y, int w, int h) {
        press_x_ = x; press_y_ = y; press_w_ = w; press_h_ = h;
        press_started_ms_ = SDL_GetTicks();
    }
    bool save_png(const std::string& path);
    bool window_to_logical(int window_x, int window_y, int& logical_x, int& logical_y) const;
    void draw_title(const std::vector<std::string>& items, int cursor,
                    bool continue_enabled = true);
    void draw_pause_menu(const std::vector<std::string>& items, int cursor);
    void draw_save_menu(const std::vector<SaveSlotInfo>& slots, int cursor, bool save_mode);
    void draw_settings(const UserSettings& settings, int cursor, int page);
    void draw_gallery(const std::vector<GalleryItem>& items, int cursor, int tab = 0,
                      int bgm_cursor = 0,
                      const std::vector<std::string>& bgm_tracks = {});
    void draw_cg_view(const std::string& relative_path);
    void draw_brand(const std::string& relative_path, bool white);
    void toggle_debug() { debug_ = !debug_; }
    bool debug() const { return debug_; }
    void set_user_preferences(bool effects_enabled, Uint8 textbox_alpha) {
        effects_enabled_ = effects_enabled;
        textbox_alpha_ = textbox_alpha;
    }
    void set_text_reveal(std::size_t chars) { text_reveal_ = chars; }
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
    TTF_Font* menu_font_ = nullptr;
    TTF_Font* osd_font_ = nullptr;
    bool debug_ = false;
    bool effects_enabled_ = true;
    Uint8 textbox_alpha_ = 255;
    float fade_alpha_ = 0.0f;
    int press_x_ = 0;
    int press_y_ = 0;
    int press_w_ = 0;
    int press_h_ = 0;
    Uint32 press_started_ms_ = 0;
    std::size_t text_reveal_ = static_cast<std::size_t>(-1);

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
    void draw_message_buttons();
    void draw_select(const GameSnapshot& snapshot);
    void draw_osd(const GameSnapshot& snapshot);

    void draw_text(const std::string& text, TTF_Font* font, SDL_Color color,
                   SDL_Color outline, int x, int y, int wrap_width,
                   int line_height);
    void draw_sprite(const std::string& relative, int x, int y, int w, int h,
                     int source_y = 0);
    static std::vector<std::string> wrap_text(TTF_Font* font, const std::string& text,
                                              int width);
    static SDL_Color parse_color(const std::string& value, SDL_Color fallback);
    static std::string path_of(const AssetManager& assets, const std::string& relative);

    // Text is re-rendered only when its content or style changes; every frame
    // just blits one cached texture per line.  Re-rasterising glyphs (with an
    // 8-direction outline) on every frame is far too slow for the Switch CPU
    // and made lines drop out for a frame.
    struct TextEntry {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
        std::list<std::string>::iterator lru;
    };
    SDL_Texture* text_texture(TTF_Font* font, const std::string& text,
                              SDL_Color color, SDL_Color outline,
                              int* width, int* height);
    std::vector<std::string> wrap_text_cached(TTF_Font* font, const std::string& text,
                                              int width);
    void draw_outlined(TTF_Font* font, const std::string& text, SDL_Color color,
                       SDL_Color outline, int x, int y);
    void clear_text_cache();

    std::list<std::string> text_lru_;
    std::unordered_map<std::string, TextEntry> text_cache_;
    std::unordered_map<std::string, std::vector<std::string>> wrap_cache_;
};

}  // namespace artc
