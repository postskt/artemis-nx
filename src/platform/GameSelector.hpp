#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_ttf.h>

namespace artc {

struct GameEntry {
    std::string title;
    std::filesystem::path root;
    std::filesystem::path config;
};

std::vector<GameEntry> scan_games(const std::filesystem::path& games_dir);

class GameSelector {
public:
    GameSelector(SDL_Window* window, const std::filesystem::path& font_path,
                 int logical_width, int logical_height);
    ~GameSelector();

    void draw(const std::vector<GameEntry>& games, int cursor);
    void present();

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* title_font_ = nullptr;
    TTF_Font* row_font_ = nullptr;
    int width_ = 1024;
    int height_ = 768;

    void text(const std::string& value, TTF_Font* font, SDL_Color color, int x, int y);
};

}  // namespace artc
