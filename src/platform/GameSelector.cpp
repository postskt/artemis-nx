#include "GameSelector.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "Config.hpp"

namespace artc {

std::vector<GameEntry> scan_games(const std::filesystem::path& games_dir) {
    std::vector<GameEntry> games;
    std::error_code ec;
    if (!std::filesystem::is_directory(games_dir, ec)) return games;
    for (const auto& entry : std::filesystem::directory_iterator(
             games_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }
        const auto config = entry.path() / "runner_config.json";
        if (!std::filesystem::is_regular_file(config, ec)) {
            ec.clear();
            continue;
        }
        try {
            Config loaded = Config::load(config);
            games.push_back(GameEntry{loaded.window_title(), entry.path(), config});
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[launcher] skip %s: %s\n",
                         entry.path().string().c_str(), e.what());
        }
    }
    std::sort(games.begin(), games.end(),
              [](const GameEntry& a, const GameEntry& b) { return a.title < b.title; });
    return games;
}

GameSelector::GameSelector(SDL_Window* window, const std::filesystem::path& font_path,
                           int logical_width, int logical_height)
    : window_(window), width_(logical_width), height_(logical_height) {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) throw std::runtime_error(std::string("selector renderer: ") + SDL_GetError());
    SDL_RenderSetLogicalSize(renderer_, width_, height_);
    title_font_ = TTF_OpenFont(font_path.string().c_str(), 48);
    row_font_ = TTF_OpenFont(font_path.string().c_str(), 30);
    if (!title_font_ || !row_font_) {
        throw std::runtime_error(std::string("selector font: ") + TTF_GetError());
    }
}

GameSelector::~GameSelector() {
    if (title_font_) TTF_CloseFont(title_font_);
    if (row_font_) TTF_CloseFont(row_font_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
}

void GameSelector::text(const std::string& value, TTF_Font* font, SDL_Color color,
                        int x, int y) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, value.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    if (!texture) return;
    int width = 0;
    int height = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
    SDL_Rect dst{x, y, width, height};
    SDL_RenderCopy(renderer_, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

void GameSelector::draw(const std::vector<GameEntry>& games, int cursor) {
    SDL_SetRenderDrawColor(renderer_, 12, 14, 22, 255);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 35, 42, 58, 255);
    SDL_Rect header{0, 0, width_, 120};
    SDL_RenderFillRect(renderer_, &header);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

    text("ARTEMIS NX", title_font_, SDL_Color{238, 242, 255, 255}, 64, 28);
    text("ゲームを選択  /  SELECT GAME", row_font_, SDL_Color{170, 190, 220, 255}, 66, 88);

    if (games.empty()) {
        text("No games found under sdmc:/switch/artemis_runner/games/",
             row_font_, SDL_Color{255, 160, 160, 255}, 64, 220);
        return;
    }

    const int visible = std::min<int>(static_cast<int>(games.size()), 8);
    const int first = std::max(0, std::min(cursor - 3,
                                           static_cast<int>(games.size()) - visible));
    const int row_height = 66;
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        const int y = 170 + i * row_height;
        SDL_Rect row{54, y - 8, width_ - 108, row_height - 8};
        if (index == cursor) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 178, 72, 112, 210);
            SDL_RenderFillRect(renderer_, &row);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        }
        const std::string label = std::to_string(index + 1) + ". " + games[index].title;
        text(label, row_font_, SDL_Color{245, 245, 250, 255}, 78, y + 4);
    }
    text("D-Pad / Left Stick / Arrow: select    A / Enter / Touch: launch",
         row_font_, SDL_Color{145, 160, 185, 255}, 64, height_ - 66);
}

void GameSelector::present() {
    SDL_RenderPresent(renderer_);
}

}  // namespace artc
