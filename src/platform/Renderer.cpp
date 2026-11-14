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
    const int pad = 2;
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
        const int offsets[][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2},
                                  {-2, -2}, {2, 2}, {-2, 2}, {2, -2}};
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
        body_left_ = font_int(adv, "left", body_left_);
        body_top_ = font_int(adv, "top", body_top_);
        body_width_ = font_int(adv, "width", body_width_);
        body_color_ = font_color(adv, "color", body_color_);
        body_outline_ = font_color(adv, "outlinecolor", body_outline_);
    }
    if (layout.contains("name01") && layout["name01"].is_object()) {
        const auto& name = layout["name01"];
        name_size_ = font_int(name, "size", name_size_);
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
    if (snapshot.select.active) draw_select(snapshot);
    if (debug_) draw_osd(snapshot);
}

void Renderer::present() {
    SDL_RenderPresent(renderer_);
}

bool Renderer::save_png(const std::string& path) {
    const int width = config_.logical_width();
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

void Renderer::draw_title(const std::vector<std::string>& items, int cursor) {
    SDL_SetRenderDrawColor(renderer_, 8, 10, 18, 255);
    SDL_RenderClear(renderer_);

    const std::string base = config_.resolve_magic(":ui/");
    if (SDL_Texture* background = textures_->get(path_of(assets_, base + "title/bg.png"))) {
        SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderCopy(renderer_, background, nullptr, &dst);
    }
    // The title UI directory also contains a trial-version Raspberry Cube logo
    // in some Windows data sets.  Prefer the game's own CG logo.
    SDL_Texture* logo_texture = textures_->get(
        path_of(assets_, config_.resolve_magic(":cg/") + "logo.png"));
    if (!logo_texture) {
        logo_texture = textures_->get(path_of(assets_, base + "title/logo.png"));
    }
    if (logo_texture) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(logo_texture, nullptr, nullptr, &width, &height);
        const int max_width = config_.logical_width() * 3 / 5;
        if (width < max_width) {
            const double scale = static_cast<double>(max_width) / width;
            width = max_width;
            height = static_cast<int>(height * scale);
        }
        SDL_Rect dst{(config_.logical_width() - width) / 2, 70, width, height};
        SDL_RenderCopy(renderer_, logo_texture, nullptr, &dst);
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    const int start_y = items.size() > 6 ? 360 : 430;
    const int step_y = items.size() > 6 ? 42 : 48;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int y = start_y + static_cast<int>(i) * step_y;
        if (static_cast<int>(i) == cursor) {
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 190);
            SDL_Rect highlight{config_.logical_width() / 2 - 220, y - 7, 440, 42};
            SDL_RenderFillRect(renderer_, &highlight);
        }
        draw_outlined(menu_font_, items[i], SDL_Color{255, 255, 255, 255},
                      SDL_Color{35, 20, 30, 255}, config_.logical_width() / 2 - 190, y);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_save_menu(const std::vector<SaveSlotInfo>& slots, int cursor,
                              bool save_mode) {
    SDL_SetRenderDrawColor(renderer_, 8, 10, 18, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    if (SDL_Texture* background = textures_->get(
            path_of(assets_, base + (save_mode ? "save/bg_save.png" : "save/bg_load.png")))) {
        SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderCopy(renderer_, background, nullptr, &dst);
    }

    draw_outlined(select_font_, save_mode ? "セーブ" : "ロード",
                  SDL_Color{255, 255, 255, 255}, SDL_Color{35, 20, 30, 255}, 60, 35);

    const int cell_width = 450;
    const int cell_height = 120;
    const int origin_x = 48;
    const int origin_y = 115;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 8 && i < static_cast<int>(slots.size()); ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int x = origin_x + column * 500;
        const int y = origin_y + row * cell_height;
        SDL_Rect cell{x, y, cell_width, cell_height - 12};
        if (i == cursor) {
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 180);
            SDL_RenderFillRect(renderer_, &cell);
        }
        SDL_SetRenderDrawColor(renderer_, 70, 78, 96, 220);
        SDL_RenderDrawRect(renderer_, &cell);

        const auto& slot = slots[i];
        SDL_Rect thumb{x + 8, y + 8, 160, 90};
        if (slot.exists && !slot.thumbnail.empty()) {
            if (SDL_Texture* preview = textures_->get(slot.thumbnail)) {
                SDL_RenderCopy(renderer_, preview, nullptr, &thumb);
            }
        } else {
            SDL_SetRenderDrawColor(renderer_, 35, 38, 48, 255);
            SDL_RenderFillRect(renderer_, &thumb);
        }
        const std::string number = "SLOT " + std::to_string(slot.slot);
        draw_outlined(menu_font_, number, SDL_Color{220, 225, 240, 255},
                      SDL_Color{25, 20, 30, 255}, x + 180, y + 10);
        const std::string title = slot.exists
                                      ? (slot.title.empty() ? slot.script : slot.title)
                                      : "EMPTY";
        draw_outlined(menu_font_, title, SDL_Color{255, 255, 255, 255},
                      SDL_Color{25, 20, 30, 255}, x + 180, y + 44);
        draw_outlined(menu_font_, slot.saved_at, SDL_Color{190, 200, 220, 255},
                      SDL_Color{25, 20, 30, 255}, x + 180, y + 78);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_settings(const UserSettings& settings, int cursor) {
    SDL_SetRenderDrawColor(renderer_, 18, 20, 30, 255);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 42, 46, 62, 235);
    SDL_Rect panel{210, 130, 620, 470};
    SDL_RenderFillRect(renderer_, &panel);
    SDL_SetRenderDrawColor(renderer_, 110, 118, 145, 220);
    SDL_RenderDrawRect(renderer_, &panel);
    draw_outlined(select_font_, "設定", SDL_Color{255, 255, 255, 255},
                  SDL_Color{35, 20, 30, 255}, 60, 35);

    const std::vector<std::string> labels = {
        "文字速度", "オート速度", "BGM音量", "SE音量", "ボイス音量", "全画面",
    };
    char values[6][24]{};
    std::snprintf(values[0], sizeof(values[0]), "%.1fx", settings.text_speed);
    std::snprintf(values[1], sizeof(values[1]), "%.1fx", settings.auto_speed);
    std::snprintf(values[2], sizeof(values[2]), "%d%%",
                  static_cast<int>(settings.bgm_volume * 100));
    std::snprintf(values[3], sizeof(values[3]), "%d%%",
                  static_cast<int>(settings.se_volume * 100));
    std::snprintf(values[4], sizeof(values[4]), "%d%%",
                  static_cast<int>(settings.voice_volume * 100));
    std::snprintf(values[5], sizeof(values[5]), "%s", settings.fullscreen ? "ON" : "OFF");

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 6; ++i) {
        const int y = 190 + i * 66;
        if (i == cursor) {
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 180);
            SDL_Rect highlight{250, y - 10, 520, 52};
            SDL_RenderFillRect(renderer_, &highlight);
        }
        draw_outlined(menu_font_, labels[i], SDL_Color{255, 255, 255, 255},
                      SDL_Color{35, 20, 30, 255}, 300, y);
        draw_outlined(menu_font_, values[i], SDL_Color{240, 220, 180, 255},
                      SDL_Color{35, 20, 30, 255}, 620, y);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_gallery(const std::vector<GalleryItem>& items, int cursor) {
    SDL_SetRenderDrawColor(renderer_, 8, 10, 18, 255);
    SDL_RenderClear(renderer_);
    const std::string base = config_.resolve_magic(":ui/");
    if (SDL_Texture* background = textures_->get(path_of(assets_, base + "extra/bg.png"))) {
        SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
        SDL_RenderCopy(renderer_, background, nullptr, &dst);
    }
    draw_outlined(select_font_, "CG鑑賞", SDL_Color{255, 255, 255, 255},
                  SDL_Color{35, 20, 30, 255}, 60, 35);

    const int columns = 5;
    const int cell_width = 185;
    const int cell_height = 145;
    const int origin_x = 42;
    const int origin_y = 115;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const int column = i % columns;
        const int row = i / columns;
        if (row >= 4) break;
        const int x = origin_x + column * 195;
        const int y = origin_y + row * cell_height;
        SDL_Rect cell{x, y, cell_width, cell_height - 10};
        if (i == cursor) {
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 200);
            SDL_RenderFillRect(renderer_, &cell);
        }
        SDL_SetRenderDrawColor(renderer_, 70, 78, 96, 220);
        SDL_RenderDrawRect(renderer_, &cell);
        if (items[i].unlocked) {
            if (SDL_Texture* thumb = textures_->get(path_of(assets_, items[i].thumb))) {
                SDL_Rect dst{x + 8, y + 7, cell_width - 16, 102};
                SDL_RenderCopy(renderer_, thumb, nullptr, &dst);
            }
        } else {
            draw_outlined(menu_font_, "LOCKED", SDL_Color{160, 160, 170, 255},
                          SDL_Color{25, 20, 30, 255}, x + 50, y + 40);
        }
        draw_outlined(menu_font_, items[i].id, SDL_Color{230, 235, 245, 255},
                      SDL_Color{25, 20, 30, 255}, x + 8, y + 113);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void Renderer::draw_cg_view(const std::string& relative_path) {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (!relative_path.empty()) {
        if (SDL_Texture* image = textures_->get(path_of(assets_, relative_path))) {
            SDL_Rect dst{0, 0, config_.logical_width(), config_.logical_height()};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
        }
    }
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

void Renderer::draw_textbox(const GameSnapshot& snapshot) {
    const std::string textbox_path = path_of(assets_, config_.ui_textbox());
    if (SDL_Texture* textbox = textures_->get(textbox_path)) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(textbox, nullptr, nullptr, &width, &height);
        SDL_Rect dst{0, config_.logical_height() - height, width, height};
        SDL_RenderCopy(renderer_, textbox, nullptr, &dst);
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
        draw_text(snapshot.text, body_font_, body_color_, body_outline_,
                  body_left_, body_top_, body_width_, 52);
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




