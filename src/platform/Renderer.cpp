#include "Renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include <SDL_image.h>

namespace artc {

namespace {

int font_int(const nlohmann::json& layout, const char* key, int fallback) {
    if (layout.is_object() && layout.contains(key) && layout[key].is_number_integer()) {
        return layout[key].get<int>();
    }
    return fallback;
}

SDL_Color font_color(const nlohmann::json& layout, const char* key, SDL_Color fallback) {
    if (!layout.is_object() || !layout.contains(key) || !layout[key].is_string()) {
        return fallback;
    }
    const std::string value = layout[key].get<std::string>();
    try {
        const unsigned long parsed = std::stoul(value, nullptr, 0);
        return SDL_Color{
            static_cast<Uint8>((parsed >> 16) & 0xFF),
            static_cast<Uint8>((parsed >> 8) & 0xFF),
            static_cast<Uint8>(parsed & 0xFF),
            255,
        };
    } catch (...) {
        return fallback;
    }
}

std::size_t utf8_char_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

std::string utf8_prefix(const std::string& text, std::size_t count) {
    std::size_t offset = 0;
    std::size_t seen = 0;
    while (offset < text.size() && seen < count) {
        offset += utf8_char_len(static_cast<unsigned char>(text[offset]));
        ++seen;
    }
    return text.substr(0, std::min(offset, text.size()));
}

}  // namespace

TextureCache::TextureCache(SDL_Renderer* renderer, std::size_t capacity)
    : renderer_(renderer), capacity_(capacity == 0 ? 1 : capacity) {}

TextureCache::~TextureCache() {
    clear();
}

SDL_Texture* TextureCache::get(const std::filesystem::path& path) {
    const std::string key = path.string();
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        ++hits_;
        return found->second.texture;
    }
    ++misses_;
    SDL_Texture* texture = IMG_LoadTexture(renderer_, key.c_str());
    if (!texture) {
        std::fprintf(stderr, "[texture] load failed: %s: %s\n", key.c_str(), IMG_GetError());
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    if (entries_.size() >= capacity_) {
        const std::string victim = lru_.back();
        lru_.pop_back();
        auto it = entries_.find(victim);
        if (it != entries_.end()) {
            SDL_DestroyTexture(it->second.texture);
            entries_.erase(it);
        }
    }
    lru_.push_front(key);
    entries_[key] = Entry{texture, lru_.begin()};
    return texture;
}

void TextureCache::clear() {
    for (auto& [key, entry] : entries_) {
        (void)key;
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    entries_.clear();
    lru_.clear();
}

Renderer::Renderer(SDL_Window* window, const Config& config, const AssetManager& assets)
    : config_(config), assets_(assets), window_(window), textures_(nullptr) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer_) {
        throw std::runtime_error(std::string("cannot create SDL renderer: ") + SDL_GetError());
    }
    // Rebind the cache to the renderer created above.
    textures_ = std::make_unique<TextureCache>(renderer_, 64);
    SDL_RenderSetLogicalSize(renderer_, config_.logical_width(), config_.logical_height());
    load_layout();

    const std::string font_path = (assets_.root() / config_.default_font()).string();
    body_font_ = TTF_OpenFont(font_path.c_str(), body_size_);
    name_font_ = TTF_OpenFont(font_path.c_str(), name_size_);
    select_font_ = TTF_OpenFont(font_path.c_str(), 42);
    menu_font_ = TTF_OpenFont(font_path.c_str(), 28);
    osd_font_ = TTF_OpenFont(font_path.c_str(), 16);
    if (!body_font_ || !name_font_ || !select_font_ || !menu_font_ || !osd_font_) {
        throw std::runtime_error(std::string("cannot open font: ") + TTF_GetError());
    }
}

Renderer::~Renderer() {
    clear_text_cache();
    textures_->clear();
    if (body_font_) TTF_CloseFont(body_font_);
    if (name_font_) TTF_CloseFont(name_font_);
    if (select_font_) TTF_CloseFont(select_font_);
    if (menu_font_) TTF_CloseFont(menu_font_);
    if (osd_font_) TTF_CloseFont(osd_font_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
}

void Renderer::clear_text_cache() {
    for (auto& [key, entry] : text_cache_) {
        (void)key;
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    text_cache_.clear();
    text_lru_.clear();
    wrap_cache_.clear();
}

SDL_Texture* Renderer::text_texture(TTF_Font* font, const std::string& text,
                                    SDL_Color color, SDL_Color outline,
                                    int* width, int* height) {
    if (text.empty() || !font) return nullptr;
    const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(font)) + "|" +
                            std::to_string(color.r) + "," + std::to_string(color.g) + "," +
                            std::to_string(color.b) + "|" + std::to_string(outline.r) + "," +
                            std::to_string(outline.g) + "," + std::to_string(outline.b) + "|" +
                            text;
    auto found = text_cache_.find(key);
    if (found != text_cache_.end()) {
        text_lru_.splice(text_lru_.begin(), text_lru_, found->second.lru);
        if (width) *width = found->second.width;
        if (height) *height = found->second.height;
        return found->second.texture;
    }

    SDL_Surface* outline_surface = TTF_RenderUTF8_Blended(font, text.c_str(), outline);
    SDL_Surface* fill_surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!fill_surface) {
        if (outline_surface) SDL_FreeSurface(outline_surface);
        return nullptr;
    }
    const int pad = 1;
    SDL_Surface* composed = SDL_CreateRGBSurfaceWithFormat(
        0, fill_surface->w + pad * 2, fill_surface->h + pad * 2, 32,
        SDL_PIXELFORMAT_ARGB8888);
    if (!composed) {
        SDL_FreeSurface(fill_surface);
        if (outline_surface) SDL_FreeSurface(outline_surface);
        return nullptr;
    }
    SDL_SetSurfaceBlendMode(composed, SDL_BLENDMODE_NONE);
    SDL_FillRect(composed, nullptr, SDL_MapRGBA(composed->format, 0, 0, 0, 0));
    SDL_SetSurfaceBlendMode(composed, SDL_BLENDMODE_BLEND);
    if (outline_surface) {
        SDL_SetSurfaceBlendMode(outline_surface, SDL_BLENDMODE_BLEND);
        const int offsets[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                  {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
        for (const auto& offset : offsets) {
            SDL_Rect dst{pad + offset[0], pad + offset[1], outline_surface->w,
                         outline_surface->h};
            SDL_BlitSurface(outline_surface, nullptr, composed, &dst);
        }
        SDL_FreeSurface(outline_surface);
    }
    {
        SDL_SetSurfaceBlendMode(fill_surface, SDL_BLENDMODE_BLEND);
        SDL_Rect dst{pad, pad, fill_surface->w, fill_surface->h};
        SDL_BlitSurface(fill_surface, nullptr, composed, &dst);
    }
    SDL_FreeSurface(fill_surface);

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, composed);
    int tex_w = composed->w;
    int tex_h = composed->h;
    SDL_FreeSurface(composed);
    if (!texture) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    if (text_cache_.size() >= 160 && !text_lru_.empty()) {
        const std::string victim = text_lru_.back();
        text_lru_.pop_back();
        auto it = text_cache_.find(victim);
        if (it != text_cache_.end()) {
            if (it->second.texture) SDL_DestroyTexture(it->second.texture);
            text_cache_.erase(it);
        }
    }
    text_lru_.push_front(key);
    text_cache_[key] = TextEntry{texture, tex_w, tex_h, text_lru_.begin()};
    if (width) *width = tex_w;
    if (height) *height = tex_h;
    return texture;
}

void Renderer::draw_outlined(TTF_Font* font, const std::string& text, SDL_Color color,
                             SDL_Color outline, int x, int y) {
    int width = 0;
    int height = 0;
    SDL_Texture* texture = text_texture(font, text, color, outline, &width, &height);
    if (!texture) return;
    SDL_Rect dst{x - 2, y - 2, width, height};
    SDL_RenderCopy(renderer_, texture, nullptr, &dst);
}

std::vector<std::string> Renderer::wrap_text_cached(TTF_Font* font, const std::string& text,
                                                    int width) {
    const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(font)) + "|" +
                            std::to_string(width) + "|" + text;
    auto found = wrap_cache_.find(key);
    if (found != wrap_cache_.end()) return found->second;
    std::vector<std::string> lines = wrap_text(font, text, width);
    if (wrap_cache_.size() > 64) wrap_cache_.clear();
    wrap_cache_[key] = lines;
    return lines;
}

void Renderer::load_layout() {
    const auto& layout = config_.font_layout();
    if (layout.contains("adv01") && layout["adv01"].is_object()) {
        const auto& adv = layout["adv01"];
        body_size_ = font_int(adv, "size", body_size_);
        body_size_ = std::max(28, body_size_ - 5);
        body_left_ = font_int(adv, "left", body_left_);
        body_top_ = font_int(adv, "top", body_top_);
        body_width_ = font_int(adv, "width", body_width_);
        body_color_ = font_color(adv, "color", body_color_);
        body_outline_ = font_color(adv, "outlinecolor", body_outline_);
    }
    if (layout.contains("name01") && layout["name01"].is_object()) {
        const auto& name = layout["name01"];
        name_size_ = font_int(name, "size", name_size_);
        name_size_ = std::max(26, name_size_ - 3);
        name_left_ = font_int(name, "left", name_left_);
        name_top_ = font_int(name, "top", name_top_);
        name_color_ = font_color(name, "color", name_color_);
        name_outline_ = font_color(name, "outlinecolor", name_outline_);
    }
}

void Renderer::draw(const GameSnapshot& snapshot) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    draw_background(snapshot);
    draw_characters(snapshot);
    draw_filters(snapshot);
    if (snapshot.textbox_visible) draw_textbox(snapshot);
    if (snapshot.textbox_visible) draw_message_buttons();
    if (snapshot.select.active) draw_select(snapshot);
    if (debug_) draw_osd(snapshot);
}

void Renderer::present() {
    if (press_w_ > 0 && press_h_ > 0) {
        const Uint32 age = SDL_GetTicks() - press_started_ms_;
        if (age < 160) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            const Uint8 alpha = static_cast<Uint8>((1.0f - age / 160.0f) * 110.0f);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, alpha);
            SDL_Rect press{press_x_, press_y_, press_w_, press_h_};
            SDL_RenderFillRect(renderer_, &press);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        }
    }
    if (fade_alpha_ > 0.001f) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        const Uint8 alpha = static_cast<Uint8>(std::clamp(fade_alpha_, 0.0f, 1.0f) * 255.0f);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, alpha);
        SDL_Rect full{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderFillRect(renderer_, &full);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }
    SDL_RenderPresent(renderer_);
}

bool Renderer::window_to_logical(int window_x, int window_y,
                                 int& logical_x, int& logical_y) const {
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(window_, &window_width, &window_height);
    const int logical_width = config_.logical_width();
    const int logical_height = config_.logical_height();
    if (window_width <= 0 || window_height <= 0) return false;
    const double scale = std::min(static_cast<double>(window_width) / logical_width,
                                  static_cast<double>(window_height) / logical_height);
    const double view_width = logical_width * scale;
    const double view_height = logical_height * scale;
    const double offset_x = (window_width - view_width) * 0.5;
    const double offset_y = (window_height - view_height) * 0.5;
    if (window_x < offset_x || window_y < offset_y ||
        window_x >= offset_x + view_width || window_y >= offset_y + view_height) {
        return false;
    }
    logical_x = static_cast<int>((window_x - offset_x) / scale);
    logical_y = static_cast<int>((window_y - offset_y) / scale);
    return true;
}

bool Renderer::save_png(const std::string& path) {    const int width = config_.logical_width();
    const int height = config_.logical_height();
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return false;
    const int result = SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                            surface->pixels, surface->pitch);
    if (result != 0) {
        SDL_FreeSurface(surface);
        return false;
    }
    const bool ok = IMG_SavePNG(surface, path.c_str()) == 0;
    SDL_FreeSurface(surface);
    return ok;
}

void Renderer::draw_sprite(const std::string& relative, int x, int y, int w, int h,
                           int source_y) {
    if (relative.empty()) return;
    std::string path = relative;
    if (path.size() < 4 || path.substr(path.size() - 4) != ".png") path += ".png";
    if (SDL_Texture* texture = textures_->get(path_of(assets_, path))) {
        SDL_Rect src{0, source_y, w, h};
        SDL_Rect dst{x, y, w, h};
        SDL_RenderCopy(renderer_, texture, &src, &dst);
    }
}

void Renderer::draw_title(const std::vector<std::string>& items, int cursor,
                          bool continue_enabled) {
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[title] begin\n");
        logged = true;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + "title/bg", 0, 0, 1024, 768);
    std::fprintf(stderr, "[title] bg done\n");

    struct Button {
        const char* asset;
        int x;
        int y;
        int w;
        int h;
    };
    static const Button buttons[] = {
        {"title/bt_start", 736, 329, 150, 48},
        {"title/bt_continue", 736, 393, 207, 48},
        {"title/bt_load", 736, 456, 137, 49},
        {"title/bt_config", 736, 520, 168, 48},
        {"title/bt_extra", 736, 583, 177, 48},
        {"title/bt_exit", 687, 647, 302, 94},
    };
    for (int i = 0; i < 6 && i < static_cast<int>(items.size()); ++i) {
        int source_y = i == cursor ? (i == 5 ? 94 : 48) : 0;
        if (i == 1 && !continue_enabled) source_y = 96;
        std::fprintf(stderr, "[title] button %d %s\n", i, buttons[i].asset);
        draw_sprite(base + buttons[i].asset, buttons[i].x, buttons[i].y,
                    buttons[i].w, buttons[i].h, source_y);
    }
    std::fprintf(stderr, "[title] done\n");
}

void Renderer::draw_pause_menu(const std::vector<std::string>& items, int cursor) {
    SDL_SetRenderDrawColor(renderer_, 10, 10, 16, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + "title/bg", 0, 0, 1024, 768);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
    SDL_Rect shade{0, 0, 1024, 768};
    SDL_RenderFillRect(renderer_, &shade);
    SDL_SetRenderDrawColor(renderer_, 30, 32, 45, 235);
    SDL_Rect panel{250, 120, 524, 450};
    SDL_RenderFillRect(renderer_, &panel);
    draw_outlined(select_font_, "PAUSE", SDL_Color{255, 255, 255, 255},
                  SDL_Color{35, 20, 30, 255}, 430, 145);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int y = 235 + static_cast<int>(i) * 52;
        if (static_cast<int>(i) == cursor) {
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 190);
            SDL_Rect highlight{330, y - 7, 364, 44};
            SDL_RenderFillRect(renderer_, &highlight);
        }
        draw_outlined(menu_font_, items[i], SDL_Color{255, 255, 255, 255},
                      SDL_Color{25, 20, 30, 255}, 360, y);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_save_menu(const std::vector<SaveSlotInfo>& slots, int cursor,
                              bool save_mode) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + (save_mode ? "save/bg_save" : "save/bg_load"),
                0, 0, 1024, 768);

    static const int xs[] = {36, 355, 674};
    static const int ys[] = {145, 281, 417, 551};
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 12 && i < static_cast<int>(slots.size()); ++i) {
        const int x = xs[i % 3];
        const int y = ys[i / 3];
        draw_sprite(base + "save/bt_slot", x, y, 316, 132, i == cursor ? 132 : 0);
        const auto& slot = slots[i];
        SDL_Rect thumb{x + 8, y + 8, 160, 90};
        if (slot.exists && !slot.thumbnail.empty()) {
            if (SDL_Texture* preview = textures_->get(slot.thumbnail)) {
                SDL_RenderCopy(renderer_, preview, nullptr, &thumb);
            }
        }
        const std::string number = "SLOT " + std::to_string(slot.slot);
        draw_outlined(menu_font_, number, SDL_Color{110, 70, 55, 255},
                      SDL_Color{255, 245, 235, 255}, x + 176, y + 12);
        const std::string title = slot.exists
                                      ? (slot.title.empty() ? slot.script : slot.title)
                                      : "EMPTY";
        draw_outlined(menu_font_, title, SDL_Color{90, 60, 50, 255},
                      SDL_Color{255, 245, 235, 255}, x + 176, y + 46);
        draw_outlined(osd_font_, slot.saved_at, SDL_Color{125, 100, 90, 255},
                      SDL_Color{255, 245, 235, 255}, x + 176, y + 82);
    }

    // Page controls and bottom navigation use the original sheet coordinates.
    draw_sprite(base + "save/bt_pagetop", 631, 94, 27, 30);
    draw_sprite(base + "save/bt_pageprev", 673, 94, 20, 30);
    draw_sprite(base + "save/bt_pagenext", 922, 94, 20, 30);
    draw_sprite(base + "save/bt_pageend", 957, 94, 27, 30);
    draw_sprite(base + "save/bt_save", 516, 726, 56, 34, save_mode ? 34 : 0);
    draw_sprite(base + "save/bt_load", 625, 727, 56, 33, save_mode ? 0 : 33);
    draw_sprite(base + "save/bt_qload", 733, 727, 100, 33);
    draw_sprite(base + "save/bt_return", 886, 727, 79, 33);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_settings(const UserSettings& settings, int cursor, int page) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    // The source pin is 20x22.  Drawing it at 9x24 stretched it into a
    // half-circle; keep its native aspect and center it on the 24px track.
    auto draw_slider = [&](int x, int y, float value) {
        draw_sprite(base + "conf/bt_slider", x, y, 248, 24, 0);
        const int pin_x = x + static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 228.0f) - 10;
        draw_sprite(base + "conf/pin", pin_x, y - 1, 20, 22);
    };
    if (page <= 1) {
        draw_sprite(base + "conf/bg01", 0, 0, 1024, 768);
        draw_sprite(base + "conf/bt_system", 213, 60, 207, 47, 94);
        draw_sprite(base + "conf/bt_sound", 421, 60, 203, 47);
        draw_sprite(base + "conf/bt_guide", 625, 60, 190, 47);
        draw_sprite(base + "conf/bt_screen", 413, 137, 139, 23, settings.fullscreen ? 46 : 0);
        draw_sprite(base + "conf/bt_window", 241, 137, 153, 23, settings.fullscreen ? 0 : 46);
        draw_sprite(base + "conf/bt_on", 241, 207, 63, 23, settings.effect ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 413, 207, 70, 23, settings.effect ? 0 : 46);
        draw_slider(269, 280, settings.sysse_volume);
        draw_slider(196, 375, settings.text_speed / 3.0f);
        draw_slider(196, 450, settings.auto_speed / 3.0f);
        draw_slider(196, 525, settings.window_alpha);
        draw_sprite(base + "conf/bt_on", 743, 371, 63, 23, settings.text_color ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 865, 371, 70, 23, settings.text_color ? 0 : 46);
        draw_sprite(base + "conf/bt_on", 743, 446, 63, 23, settings.skip_unread ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 865, 446, 70, 23, settings.skip_unread ? 0 : 46);
        draw_sprite(base + "conf/sample", 36, 589, 449, 106);
    } else if (page == 2) {
        draw_sprite(base + "conf/bg02", 0, 0, 1024, 768);
        draw_sprite(base + "conf/bt_system", 213, 60, 207, 47);
        draw_sprite(base + "conf/bt_sound", 421, 60, 203, 47, 94);
        draw_sprite(base + "conf/bt_guide", 625, 60, 190, 47);
        draw_slider(196, 201, settings.master_volume);
        draw_slider(196, 301, settings.bgm_volume);
        draw_slider(196, 401, settings.se_volume);
        draw_slider(196, 501, settings.voice_volume);
        draw_slider(196, 601, settings.sysse_volume);
        draw_slider(698, 201, settings.bgm_voice_fade ? 1.0f : 0.0f);
        draw_sprite(base + "conf/bt_on", 743, 297, 63, 23, settings.bgm_voice_fade ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 865, 297, 70, 23, settings.bgm_voice_fade ? 0 : 46);
        draw_sprite(base + "conf/bt_on", 743, 397, 63, 23, settings.voice_skip ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 865, 397, 70, 23, settings.voice_skip ? 0 : 46);
        draw_sprite(base + "conf/bt_on", 743, 497, 63, 23, settings.auto_stop ? 46 : 0);
        draw_sprite(base + "conf/bt_off", 865, 497, 70, 23, settings.auto_stop ? 0 : 46);
        draw_sprite(base + "conf/bt_mute", 421, 223, 65, 20);
        draw_sprite(base + "conf/bt_mute", 421, 323, 65, 20);
        draw_sprite(base + "conf/bt_mute", 421, 423, 65, 20);
        draw_sprite(base + "conf/bt_mute", 421, 523, 65, 20);
        draw_sprite(base + "conf/bt_mute", 421, 623, 65, 20);
    } else {
        draw_sprite(base + "conf/bg03", 0, 0, 1024, 768);
        draw_sprite(base + "conf/bt_system", 213, 60, 207, 47);
        draw_sprite(base + "conf/bt_sound", 421, 60, 203, 47);
        draw_sprite(base + "conf/bt_guide", 625, 60, 190, 47, 94);
    }
    draw_sprite(base + "conf/bt_default", 484, 726, 90, 34);
    draw_sprite(base + "conf/bt_exit", 624, 727, 56, 33);
    draw_sprite(base + "conf/bt_title", 733, 727, 100, 33);
    draw_sprite(base + "conf/bt_return", 886, 727, 79, 33);

    int highlight_y = -1;
    if (page == 1) {
        static const int rows[] = {137, 207, 280, 375, 450, 525, 371, 446};
        if (cursor >= 0 && cursor < 8) highlight_y = rows[cursor];
    } else if (page == 2) {
        static const int rows[] = {201, 301, 401, 501, 601, 201};
        highlight_y = rows[std::clamp(cursor, 0, 5)];
    }
    if (highlight_y >= 0) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 255, 209, 120, 45);
        SDL_Rect highlight{180, highlight_y - 7, 760, 38};
        SDL_RenderFillRect(renderer_, &highlight);
        SDL_SetRenderDrawColor(renderer_, 190, 96, 45, 150);
        SDL_RenderDrawRect(renderer_, &highlight);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }
}

void Renderer::draw_gallery(const std::vector<GalleryItem>& items, int cursor, int tab,
                            int bgm_cursor,
                            const std::vector<std::string>& bgm_tracks) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + "extra/bg", 0, 0, 1024, 768);

    // Original SPECIAL screen: CG / SCENE / BGM tabs.  The selected tab uses
    // the 94px source row in the three tab atlases.
    draw_sprite(base + "extra/bt_excg", 129, 62, 200, 47, tab == 0 ? 94 : 0);
    draw_sprite(base + "extra/bt_exscene", 330, 62, 201, 47, tab == 1 ? 94 : 0);
    draw_sprite(base + "extra/bt_exbgm", 532, 62, 175, 47, tab == 2 ? 94 : 0);
    draw_sprite(base + "extra/bt_return", 751, 71, 109, 33);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (tab == 0) {
        static const int xs[] = {30, 275, 520, 765};
        static const int ys[] = {123, 250, 377, 504, 631};
        for (int i = 0; i < static_cast<int>(items.size()) && i < 20; ++i) {
            const int x = xs[i % 4];
            const int y = ys[i / 4];
            if (items[i].unlocked) {
                if (SDL_Texture* thumb = textures_->get(path_of(assets_, items[i].thumb))) {
                    SDL_Rect dst{x + 5, y + 5, 220, 109};
                    SDL_RenderCopy(renderer_, thumb, nullptr, &dst);
                }
            } else {
                SDL_SetRenderDrawColor(renderer_, 205, 190, 170, 220);
                SDL_Rect locked{x + 5, y + 5, 220, 109};
                SDL_RenderFillRect(renderer_, &locked);
            }
            draw_sprite(base + "extra/bt_thumb", x, y, 230, 119, i == cursor ? 119 : 0);
        }
    } else if (tab == 1) {
        static const int xs[] = {30, 520};
        static const int ys[] = {123, 249, 375, 501};
        for (int i = 0; i < static_cast<int>(items.size()) && i < 8; ++i) {
            const int x = xs[i % 2];
            const int y = ys[i / 2];
            if (items[i].unlocked) {
                if (SDL_Texture* thumb = textures_->get(path_of(assets_, items[i].thumb))) {
                    SDL_Rect dst{x + 5, y + 5, 465, 139};
                    SDL_RenderCopy(renderer_, thumb, nullptr, &dst);
                }
            } else {
                SDL_SetRenderDrawColor(renderer_, 205, 190, 170, 220);
                SDL_Rect locked{x + 5, y + 5, 465, 139};
                SDL_RenderFillRect(renderer_, &locked);
            }
            draw_sprite(base + "extra/bt_thumb_scene", x, y, 475, 149,
                        i == cursor ? 149 : 0);
        }
    } else {
        const int count = bgm_tracks.empty() ? 10 : static_cast<int>(bgm_tracks.size());
        for (int i = 0; i < count && i < 9; ++i) {
            char asset[64]{};
            std::snprintf(asset, sizeof(asset), "extra/bt_bgm%02d", i + 1);
            draw_sprite(base + asset, 315, 160 + i * 55, 395, 45,
                        i == bgm_cursor ? 45 : 0);
        }
        // Selection marker; the original atlas only contains the label art.
        if (count > 0) {
            const int y = 160 + std::clamp(bgm_cursor, 0, count - 1) * 55;
            SDL_SetRenderDrawColor(renderer_, 255, 209, 120, 64);
            SDL_Rect highlight{305, y - 4, 415, 53};
            SDL_RenderFillRect(renderer_, &highlight);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}


void Renderer::draw_brand(const std::string& relative_path, bool white) {
    SDL_SetRenderDrawColor(renderer_, white ? 255 : 0, white ? 255 : 0, white ? 255 : 0, 255);
    SDL_RenderClear(renderer_);
    if (relative_path.empty()) return;
    std::string path = relative_path;
    if (path.size() < 4 || path.substr(path.size() - 4) != ".png") path += ".png";
    if (SDL_Texture* image = textures_->get(path_of(assets_, path))) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(image, nullptr, nullptr, &width, &height);
        const int logical_width = config_.logical_width();
        const int logical_height = config_.logical_height();
        if (width >= logical_width * 3 / 4 || height >= logical_height * 3 / 4) {
            SDL_Rect dst{0, 0, logical_width, logical_height};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
        } else {
            SDL_Rect dst{(logical_width - width) / 2, (logical_height - height) / 2,
                         width, height};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
        }
    }
}

void Renderer::draw_cg_view(const std::string& relative_path) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (!relative_path.empty()) {
        if (SDL_Texture* image = textures_->get(path_of(assets_, relative_path))) {
            SDL_Rect dst{0, 0, 1024, 768};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
        }
    }
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + "extra/bt_return", 751, 71, 109, 33);
}
void Renderer::draw_background(const GameSnapshot& snapshot) {
    if (!snapshot.background.empty()) {
        const std::string path = path_of(assets_, snapshot.background);
        if (SDL_Texture* texture = textures_->get(path)) {
            SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
            SDL_RenderCopy(renderer_, texture, nullptr, &dst);
        }
    }
    for (const auto& layer : snapshot.cg_layers) {
        const std::string path = path_of(assets_, layer);
        if (SDL_Texture* texture = textures_->get(path)) {
            SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
            SDL_RenderCopy(renderer_, texture, nullptr, &dst);
        }
    }
}

void Renderer::draw_characters(const GameSnapshot& snapshot) {
    for (const auto& character : snapshot.active_fgs) {
        auto draw_texture = [&](const std::string& relative, int x, int y, double scale) {
            if (relative.empty()) return;
            SDL_Texture* texture = textures_->get(path_of(assets_, relative));
            if (!texture) return;
            int width = 0;
            int height = 0;
            SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
            SDL_Rect dst{
                x,
                y,
                static_cast<int>(width * scale),
                static_cast<int>(height * scale),
            };
            SDL_RenderCopy(renderer_, texture, nullptr, &dst);
        };
        draw_texture(character.body, character.x, character.y, character.scale);
        if (!character.face.empty()) {
            draw_texture(character.face,
                         character.x + static_cast<int>(character.face_x * character.scale),
                         character.y + static_cast<int>(character.face_y * character.scale),
                         character.scale);
        }
        for (std::size_t i = 0; i < character.ex.size() && i < character.ex_positions.size(); ++i) {
            draw_texture(character.ex[i],
                         character.x + static_cast<int>(character.ex_positions[i].first * character.scale),
                         character.y + static_cast<int>(character.ex_positions[i].second * character.scale),
                         character.scale);
        }
    }
}

void Renderer::draw_filters(const GameSnapshot& snapshot) {
    if (!effects_enabled_) return;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (snapshot.color_tone.active) {
        SDL_SetRenderDrawColor(renderer_, snapshot.color_tone.r, snapshot.color_tone.g,
                               snapshot.color_tone.b, 80);
        SDL_Rect full{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderFillRect(renderer_, &full);
    }
    if (snapshot.flash.active) {
        const int alpha = std::clamp(static_cast<int>(snapshot.flash.alpha * 255.0), 0, 255);
        SDL_SetRenderDrawColor(renderer_, snapshot.flash.r, snapshot.flash.g,
                               snapshot.flash.b, static_cast<Uint8>(alpha));
        SDL_Rect full{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderFillRect(renderer_, &full);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_message_buttons() {
    const std::string base = config_.resolve_magic(":ui/");
    draw_sprite(base + "mw/dockarea", 0, 718, 1024, 50);
    draw_sprite(base + "mw/dock", 0, 736, 1024, 32);
    struct Icon {
        const char* asset;
        int x;
        int y;
        int w;
        int h;
    };
    static const Icon icons[] = {
        {"mw/bt_lock", 703, 741, 20, 19},
        {"mw/bt_save", 413, 743, 38, 17},
        {"mw/bt_load", 477, 744, 40, 15},
        {"mw/bt_qsave", 543, 743, 54, 17},
        {"mw/bt_qload", 622, 744, 56, 15},
        {"mw/bt_auto", 745, 743, 13, 17},
        {"mw/bt_voice", 785, 742, 21, 19},
        {"mw/bt_skip", 833, 743, 16, 17},
        {"mw/bt_config", 876, 742, 17, 19},
        {"mw/bt_backlog", 920, 743, 15, 17},
        {"mw/bt_textwindow", 962, 743, 15, 17},
    };
    for (const auto& icon : icons) {
        draw_sprite(base + icon.asset, icon.x, icon.y, icon.w, icon.h);
    }
}

void Renderer::draw_textbox(const GameSnapshot& snapshot) {
    const std::string textbox_path = path_of(assets_, config_.ui_textbox());
    if (SDL_Texture* textbox = textures_->get(textbox_path)) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(textbox, nullptr, nullptr, &width, &height);
        SDL_Rect dst{0, config_.logical_height() - height, width, height};
        SDL_SetTextureAlphaMod(textbox, textbox_alpha_);
        SDL_RenderCopy(renderer_, textbox, nullptr, &dst);
        SDL_SetTextureAlphaMod(textbox, 255);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
        SDL_Rect box{0, config_.logical_height() - 190, config_.logical_width(), 190};
        SDL_RenderFillRect(renderer_, &box);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }

    if (!snapshot.speaker.empty()) {
        draw_outlined(name_font_, snapshot.speaker, name_color_, name_outline_,
                      name_left_, name_top_);
    }
    if (!snapshot.text.empty()) {
        const std::string visible = text_reveal_ == static_cast<std::size_t>(-1)
                                        ? snapshot.text
                                        : utf8_prefix(snapshot.text, text_reveal_);
        if (!visible.empty()) {
            draw_text(visible, body_font_, body_color_, body_outline_,
                      body_left_, body_top_, body_width_, body_size_ + 8);
        }
    }
}

void Renderer::draw_text(const std::string& text, TTF_Font* font, SDL_Color color,
                         SDL_Color outline, int x, int y, int wrap_width,
                         int line_height) {
    const auto lines = wrap_text_cached(font, text, wrap_width);
    int cursor = y;
    for (const auto& line : lines) {
        draw_outlined(font, line, color, outline, x, cursor);
        cursor += line_height;
    }
}

std::vector<std::string> Renderer::wrap_text(TTF_Font* font, const std::string& text,
                                             int width) {
    std::vector<std::string> lines;
    for (const auto& paragraph : [&]() {
             std::vector<std::string> result;
             std::size_t begin = 0;
             while (begin <= text.size()) {
                 const auto end = text.find('\n', begin);
                 result.push_back(text.substr(begin, end == std::string::npos ? std::string::npos
                                                                             : end - begin));
                 if (end == std::string::npos) break;
                 begin = end + 1;
             }
             return result;
         }()) {
        std::string line;
        std::size_t pos = 0;
        while (pos < paragraph.size()) {
            const auto len = utf8_char_len(static_cast<unsigned char>(paragraph[pos]));
            const std::string glyph = paragraph.substr(pos, len);
            const std::string candidate = line + glyph;
            int measured = 0;
            if (TTF_SizeUTF8(font, candidate.c_str(), &measured, nullptr) != 0) {
                measured = 0;
            }
            if (!line.empty() && measured > width) {
                lines.push_back(line);
                line = glyph;
            } else {
                line = candidate;
            }
            pos += len;
        }
        lines.push_back(line);
    }
    return lines;
}

void Renderer::draw_select(const GameSnapshot& snapshot) {
    if (!snapshot.select.active || snapshot.select.options.empty()) return;
    const int panel_width = 680;
    const int row_height = 86;
    const int panel_height = static_cast<int>(snapshot.select.options.size()) * row_height + 30;
    const int panel_x = (config_.logical_width() - panel_width) / 2;
    const int panel_y = (config_.logical_height() - panel_height) / 2;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 16, 16, 24, 190);
    SDL_Rect panel{panel_x, panel_y, panel_width, panel_height};
    SDL_RenderFillRect(renderer_, &panel);
    SDL_SetRenderDrawColor(renderer_, 230, 230, 240, 255);
    SDL_RenderDrawRect(renderer_, &panel);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

    for (std::size_t i = 0; i < snapshot.select.options.size(); ++i) {
        SDL_Rect row{panel_x + 20, panel_y + 15 + static_cast<int>(i) * row_height,
                     panel_width - 40, row_height - 8};
        if (static_cast<int>(i) == snapshot.select.cursor) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 190, 80, 120, 170);
            SDL_RenderFillRect(renderer_, &row);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        }
        const std::string text = snapshot.select.options[i].text;
        int width = 0;
        int height = 0;
        text_texture(select_font_, text, SDL_Color{255, 255, 255, 255},
                     SDL_Color{40, 20, 30, 255}, &width, &height);
        draw_outlined(select_font_, text,
                      SDL_Color{255, 255, 255, 255},
                      SDL_Color{40, 20, 30, 255},
                      panel_x + (panel_width - width) / 2,
                      row.y + (row.h - height) / 2);
    }
}

void Renderer::draw_osd(const GameSnapshot& snapshot) {
    char buffer[400];
    std::snprintf(buffer, sizeof(buffer),
                  "script=%s block=%d  fg=%zu  cache=%zu/%zu hit=%zu miss=%zu",
                  snapshot.script_name.c_str(), snapshot.block_index,
                  snapshot.active_fgs.size(), textures_->size(), textures_->capacity(),
                  textures_->hits(), textures_->misses());
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
    SDL_Rect panel{10, 10, 760, 34};
    SDL_RenderFillRect(renderer_, &panel);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    draw_outlined(osd_font_, buffer,
                  SDL_Color{235, 245, 255, 255},
                  SDL_Color{0, 0, 0, 255}, 18, 18);
}

SDL_Color Renderer::parse_color(const std::string& value, SDL_Color fallback) {
    try {
        const unsigned long parsed = std::stoul(value, nullptr, 0);
        return SDL_Color{
            static_cast<Uint8>((parsed >> 16) & 0xFF),
            static_cast<Uint8>((parsed >> 8) & 0xFF),
            static_cast<Uint8>(parsed & 0xFF),
            255,
        };
    } catch (...) {
        return fallback;
    }
}

std::string Renderer::path_of(const AssetManager& assets, const std::string& relative) {
    if (relative.empty()) return {};
    return (assets.root() / relative).string();
}

}  // namespace artc




