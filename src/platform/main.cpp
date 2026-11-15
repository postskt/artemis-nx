#include <cstdlib>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_ttf.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "AssetManager.hpp"
#include "AudioPlayer.hpp"
#include "Config.hpp"
#include "Engine.hpp"
#include "Gallery.hpp"
#include "GameSelector.hpp"
#include "InputHandler.hpp"
#include "LuaLoader.hpp"
#include "Renderer.hpp"
#include "SaveManager.hpp"
#include "Settings.hpp"

namespace {

// Boot trace: written before anything else so a crash in init is still visible.
#ifdef __SWITCH__
void tlog(const std::string& message) {
    std::ofstream log("sdmc:/artemis_boot.log", std::ios::app);
    if (log) {
        log << message << "\n";
        log.flush();
    }
}
#else
void tlog(const std::string&) {}
#endif

#ifdef __SWITCH__
void on_terminate() {
    tlog("!! std::terminate");
    std::abort();
}
#else
void on_terminate() {}
#endif

struct Options {
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path games_dir;
    std::filesystem::path save_dir;
    std::string screenshot;
    std::string ui_screenshot;
    std::string ui_screen;
    int width = 0;
    int height = 0;
    int steps = 0;
    bool root_set = false;
    bool config_set = false;
    bool games_dir_set = false;
    bool no_title = false;
};

Options parse_options(int argc, char** argv) {
    Options options;
#ifdef __SWITCH__
    options.root = "romfs:/";
    options.config = "romfs:/runner_config.json";
    options.games_dir = "sdmc:/switch/artemis_runner/games";
    options.save_dir = "sdmc:/switch/artemis_runner/saves";
#else
    options.root = "build_temp/pc_unpacked";
    options.config = "cpp_runner/runner_config.json";
    options.games_dir = "games";
    options.save_dir = "saves";
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) {
            options.root = argv[++i];
            options.root_set = true;
        } else if (arg == "--config" && i + 1 < argc) {
            options.config = argv[++i];
            options.config_set = true;
        } else if (arg == "--games-dir" && i + 1 < argc) {
            options.games_dir = argv[++i];
            options.games_dir_set = true;
        } else if (arg == "--save-dir" && i + 1 < argc) {
            options.save_dir = argv[++i];
        } else if (arg == "--no-title") {
            options.no_title = true;
        } else if (arg == "--width" && i + 1 < argc) {
            options.width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            options.height = std::atoi(argv[++i]);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            options.screenshot = argv[++i];
        } else if (arg == "--ui-screenshot" && i + 1 < argc) {
            options.ui_screenshot = argv[++i];
        } else if (arg == "--ui-screen" && i + 1 < argc) {
            options.ui_screen = argv[++i];
        } else if (arg == "--steps" && i + 1 < argc) {
            options.steps = std::atoi(argv[++i]);
        }
    }
    if (options.root_set && !options.config_set) {
        const auto candidate = options.root / "runner_config.json";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) options.config = candidate;
    }
    if (options.config_set && !options.root_set) {
        options.root = options.config.parent_path();
    }
    return options;
}

SDL_Window* create_window(const artc::Config& config, int width, int height,
                          const std::string& title) {
    SDL_Window* window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    return window;
}

int run_selector(SDL_Window* window, const std::vector<artc::GameEntry>& games,
                 const artc::Config& first_config) {
    artc::GameSelector selector(
        window,
        first_config.default_font().empty()
            ? std::filesystem::path()
            : games.front().root / first_config.default_font(),
        first_config.logical_width(), first_config.logical_height());
    artc::InputHandler input;
    int cursor = 0;
    bool selected = false;
    while (!selected) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            for (artc::Action action : input.handle_event(event)) {
                if (action == artc::Action::Quit) return -1;
                if (action == artc::Action::SelectUp) {
                    cursor = (cursor + static_cast<int>(games.size()) - 1) %
                             static_cast<int>(games.size());
                } else if (action == artc::Action::SelectDown) {
                    cursor = (cursor + 1) % static_cast<int>(games.size());
                } else if (action == artc::Action::Next ||
                           action == artc::Action::Confirm) {
                    selected = true;
                }
            }
        }
        selector.draw(games, cursor);
        selector.present();
        SDL_Delay(16);
    }
    return cursor;
}

}  // namespace

int main(int argc, char** argv) {
    std::set_terminate(on_terminate);
    tlog("=== main entry ===");
    bool romfs_mounted = false;
#ifdef __SWITCH__
#ifdef ARTC_NRO
    // The standalone NRO has no embedded RomFS; avoid touching the romfs
    // service at all (Ryujinx raises an unhandled exception for a missing one).
    romfs_mounted = false;
    tlog("ARTC_NRO build: skipping romfs");
#else
    tlog("romfsInit begin");
    romfs_mounted = R_SUCCEEDED(romfsInit());
    tlog(romfs_mounted ? "romfsInit ok" : "romfsInit FAILED");
    if (!romfs_mounted) {
        std::cerr << "[warn] romfsInit failed; starting in launcher mode\n";
    }
#endif
#endif
    const Options options = parse_options(argc, argv);
    tlog("options root=" + options.root.string() + " config=" + options.config.string());

    // Initialize SDL subsystem by subsystem so a failure names the culprit.
    struct { Uint32 flag; const char* name; } subsystems[] = {
        {SDL_INIT_VIDEO, "video"},
        {SDL_INIT_AUDIO, "audio"},
        {SDL_INIT_GAMECONTROLLER, "gamecontroller"},
        {SDL_INIT_EVENTS, "events"},
    };
    bool sdl_ok = true;
    for (const auto& sub : subsystems) {
        tlog(std::string("SDL_InitSubSystem begin: ") + sub.name);
        if (SDL_InitSubSystem(sub.flag) != 0) {
            tlog(std::string("SDL_InitSubSystem FAILED: ") + sub.name + ": " + SDL_GetError());
            sdl_ok = false;
            break;
        }
        tlog(std::string("SDL_InitSubSystem ok: ") + sub.name);
    }
    if (!sdl_ok) {
#ifdef __SWITCH__
        if (romfs_mounted) romfsExit();
#endif
        return 1;
    }
    tlog("SDL subsystem init done");

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    tlog("IMG_Init done");
    TTF_Init();
    tlog("TTF_Init done");

    int exit_code = 0;
    SDL_Window* window = nullptr;
#ifdef __SWITCH__
    std::ofstream debug_log("sdmc:/artemis_debug.log", std::ios::app);
    if (debug_log) {
        debug_log << "boot root=" << options.root << " config=" << options.config << "\n";
        debug_log.flush();
    }
#endif
    try {
        bool direct = true;
        if (options.games_dir_set) {
            direct = false;
        } else if (!options.root_set && !options.config_set) {
#ifdef ARTC_NRO
            direct = false;
#elif defined(__SWITCH__)
            direct = false;
            if (romfs_mounted) {
                std::error_code ec;
                direct = std::filesystem::is_regular_file("romfs:/runner_config.json", ec);
                if (!direct) {
                    std::ifstream probe("romfs:/runner_config.json");
                    direct = probe.good();
                }
            }
#else
            direct = true;
#endif
        }

        std::vector<artc::GameEntry> games;
        artc::GameEntry selected;
        artc::Config config;
        if (direct) {
            config = artc::Config::load(options.config);
            selected = artc::GameEntry{config.window_title(), options.root, options.config};
#ifdef __SWITCH__
            if (debug_log) {
                debug_log << "config loaded title=" << config.window_title() << "\n";
                debug_log.flush();
            }
#endif
        } else {
            games = artc::scan_games(options.games_dir);
            if (games.empty()) {
                throw std::runtime_error(
                    "no games found in " + options.games_dir.string() +
                    " (each game needs runner_config.json)");
            }
            const artc::Config first_config = artc::Config::load(games.front().config);
            const int width = options.width > 0 ? options.width : first_config.logical_width();
            const int height = options.height > 0 ? options.height : first_config.logical_height();
            window = create_window(first_config, width, height, "Artemis NX");
            int chosen = 0;
            if (options.screenshot.empty()) {
                chosen = run_selector(window, games, first_config);
                if (chosen < 0) {
                    SDL_DestroyWindow(window);
                    window = nullptr;
                    TTF_Quit();
                    IMG_Quit();
                    SDL_Quit();
#ifdef __SWITCH__
                    if (romfs_mounted) romfsExit();
#endif
                    return 0;
                }
            }
            selected = games[chosen];
            config = artc::Config::load(selected.config);
        }

        if (!window) {
            const int width = options.width > 0 ? options.width : config.logical_width();
            const int height = options.height > 0 ? options.height : config.logical_height();
            tlog("create_window begin");
            window = create_window(config, width, height, config.window_title());
            tlog("create_window ok");
        } else {
            SDL_SetWindowTitle(window, config.window_title().c_str());
            SDL_SetWindowSize(window, options.width > 0 ? options.width : config.logical_width(),
                              options.height > 0 ? options.height : config.logical_height());
        }

        artc::AssetManager assets(config, selected.root, [&](const std::string& message) {
            std::cerr << "[asset] " << message << "\n";
#ifdef __SWITCH__
            if (debug_log) debug_log << "[asset] " << message << "\n";
#endif
        });
#ifdef __SWITCH__
        if (debug_log) {
            debug_log << "assets indexed direction=" << direct << "\n";
            debug_log.flush();
        }
#endif
        artc::LuaLoader loader;
        artc::Engine engine(config, assets, loader);
        artc::Renderer renderer(window, config, assets);
        artc::AudioPlayer audio(config, assets);
        artc::InputHandler input;
        artc::SaveManager saves(options.save_dir);
        artc::UserSettings settings =
            artc::load_settings(options.save_dir / "settings.json");
        artc::Gallery gallery;
        gallery.scan(config, assets);
        gallery.load(options.save_dir / "gallery.json");
        std::vector<artc::GalleryItem> gallery_items;
        auto refresh_gallery = [&] {
            gallery_items = gallery.items();
            for (auto& item : gallery_items) item.unlocked = gallery.is_unlocked(item);
        };
        refresh_gallery();
        audio.set_volumes(settings.bgm_volume * settings.master_volume,
                          settings.se_volume * settings.master_volume,
                          settings.voice_volume * settings.master_volume);
        renderer.set_user_preferences(settings.effect,
                                      static_cast<Uint8>(settings.window_alpha * 255.0f));
        if (settings.fullscreen) SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

        enum class Screen { Brand, Title, Game, Save, Load, Pause, Settings, Gallery, CGView };
        Screen screen = (options.no_title || !options.screenshot.empty())
                            ? Screen::Game
                            : Screen::Brand;
        Screen menu_return = Screen::Game;
        int title_cursor = 0;
        int pending_title_action = -1;
        Uint32 pending_title_at = 0;
        int pause_cursor = 0;
        int slot_cursor = 0;
        int settings_cursor = 0;
        int settings_page = 1;
        int gallery_cursor = 0;
        std::string cg_view_path;
        bool save_mode = false;
        std::string pending_thumbnail;
        std::vector<artc::SaveSlotInfo> slots;
        std::vector<nlohmann::json> history;
        std::string last_event_asset;
        bool gallery_dirty = false;
        Uint32 brand_start = SDL_GetTicks();
        bool brand_voice_played = false;
        int brand_stage = -1;
        Screen previous_screen = screen;
        Uint32 screen_changed_ms = SDL_GetTicks();
        std::string last_revealed_text;
        std::size_t reveal_chars = 0;
        Uint32 reveal_tick = SDL_GetTicks();
        auto utf8_length = [](const std::string& text) {
            std::size_t count = 0;
            for (std::size_t i = 0; i < text.size();) {
                const unsigned char lead = static_cast<unsigned char>(text[i]);
                std::size_t length = 1;
                if ((lead >> 5) == 0x6) length = 2;
                else if ((lead >> 4) == 0xE) length = 3;
                else if ((lead >> 3) == 0x1E) length = 4;
                i += std::min(length, text.size() - i);
                ++count;
            }
            return count;
        };
        auto reset_text_reveal = [&] {
            last_revealed_text.clear();
            reveal_chars = 0;
            reveal_tick = SDL_GetTicks();
            renderer.set_text_reveal(static_cast<std::size_t>(-1));
        };

        auto save_current_settings = [&] {
            settings.text_speed = std::max(0.5f, std::min(3.0f, settings.text_speed));
            settings.auto_speed = std::max(0.5f, std::min(3.0f, settings.auto_speed));
            settings.master_volume = std::max(0.0f, std::min(1.0f, settings.master_volume));
            settings.bgm_volume = std::max(0.0f, std::min(1.0f, settings.bgm_volume));
            settings.se_volume = std::max(0.0f, std::min(1.0f, settings.se_volume));
            settings.voice_volume = std::max(0.0f, std::min(1.0f, settings.voice_volume));
            settings.sysse_volume = std::max(0.0f, std::min(1.0f, settings.sysse_volume));
            settings.window_alpha = std::max(0.0f, std::min(1.0f, settings.window_alpha));
            artc::save_settings(options.save_dir / "settings.json", settings);
            audio.set_volumes(settings.bgm_volume * settings.master_volume,
                              settings.se_volume * settings.master_volume,
                              settings.voice_volume * settings.master_volume);
            renderer.set_user_preferences(settings.effect,
                                          static_cast<Uint8>(settings.window_alpha * 255.0f));
            SDL_SetWindowFullscreen(window, settings.fullscreen
                                                 ? SDL_WINDOW_FULLSCREEN_DESKTOP
                                                 : 0);
        };
        auto remember_state = [&] {
            if (engine.snapshot().text.empty()) return;
            history.push_back(engine.export_state());
            constexpr std::size_t kHistoryLimit = 64;
            if (history.size() > kHistoryLimit) history.erase(history.begin());
        };
        auto advance_text = [&] {
            const std::string& text = engine.snapshot().text;
            const std::size_t total = utf8_length(text);
            if (!text.empty() && reveal_chars < total) {
                reveal_chars = total;
                renderer.set_text_reveal(reveal_chars);
                return;
            }
            remember_state();
            engine.step(artc::Action::Next);
            audio.play(engine.consume_audio());
        };
        auto restore_history = [&] {
            if (history.empty()) return;
            auto state = std::move(history.back());
            history.pop_back();
            std::string error;
            if (engine.import_state(state, &error)) {
                audio.play(engine.consume_audio());
            }
        };
        auto open_settings = [&](Screen back) {
            settings_cursor = 0;
            settings_page = 1;
            menu_return = back;
            screen = Screen::Settings;
        };
        auto open_gallery = [&](Screen back) {
            refresh_gallery();
            gallery_cursor = 0;
            menu_return = back;
            screen = Screen::Gallery;
        };

        auto refresh_slots = [&] {
            const auto list = saves.list();
            slots.assign(list.begin(), list.end());
        };
        auto play_audio = [&] {
            audio.play(engine.consume_audio());
        };
        auto play_title_bgm = [&] {
            const artc::BgmEntry* entry = config.bgm("bgm01");
            if (!entry) return;
            artc::AudioCommand command;
            command.kind = artc::AudioKind::Bgm;
            const std::string bgm_dir = config.resolve_magic(":bgm/");
            command.path = bgm_dir + entry->intro;
            command.next_path = bgm_dir + entry->loop;
            command.loop = true;
            audio.play({command});
        };
        auto start_new_game = [&] {
            audio.play({artc::AudioCommand{artc::AudioKind::BgmStop}});
            engine.start();
            play_audio();
            history.clear();
            last_event_asset.clear();
            reset_text_reveal();
            screen = Screen::Game;
        };
        auto load_slot = [&](int slot) -> bool {
            auto state = saves.load(slot);
            if (!state) return false;
            std::string error;
            if (!engine.import_state(*state, &error)) {
                std::cerr << "[save] load failed: " << error << "\n";
                return false;
            }
            audio.play({artc::AudioCommand{artc::AudioKind::BgmStop}});
            play_audio();
            history.clear();
            last_event_asset = engine.snapshot().background;
            reset_text_reveal();
            screen = Screen::Game;
            return true;
        };
        auto capture_for_save = [&] {
            std::error_code ec;
            std::filesystem::create_directories(options.save_dir, ec);
            pending_thumbnail = (options.save_dir / ".pending.png").string();
            if (!renderer.save_png(pending_thumbnail)) pending_thumbnail.clear();
        };
        auto open_save_menu = [&](Screen back) {
            capture_for_save();
            refresh_slots();
            save_mode = true;
            slot_cursor = 0;
            menu_return = back;
            screen = Screen::Save;
        };
        auto open_load_menu = [&](Screen back) {
            refresh_slots();
            save_mode = false;
            slot_cursor = 0;
            menu_return = back;
            screen = Screen::Load;
        };
        auto save_selected = [&] {
            if (slots.empty()) return;
            const int slot = slots[slot_cursor].slot;
            const auto state = engine.export_state();
            std::string thumbnail;
            if (!pending_thumbnail.empty()) {
                std::error_code ec;
                const auto target = saves.thumbnail_path(slot);
                std::filesystem::copy_file(pending_thumbnail, target,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) thumbnail = target.string();
            }
            if (saves.save(slot, state, thumbnail)) {
                refresh_slots();
                screen = Screen::Game;
            }
        };
        auto stop_game_audio = [&] {
            audio.play({artc::AudioCommand{artc::AudioKind::VoiceStop},
                        artc::AudioCommand{artc::AudioKind::BgmStop}});
        };
        auto pointer_logical = [&](int& x, int& y) {
            return renderer.window_to_logical(input.pointer_x(), input.pointer_y(), x, y);
        };
        auto title_button_at = [&](int x, int y) {
            static const int boxes[][4] = {
                {736, 329, 150, 48}, {736, 393, 207, 48}, {736, 456, 137, 49},
                {736, 520, 168, 48}, {736, 583, 177, 48}, {687, 647, 302, 94},
            };
            for (int i = 0; i < 6; ++i) {
                if (x >= boxes[i][0] && y >= boxes[i][1] &&
                    x < boxes[i][0] + boxes[i][2] && y < boxes[i][1] + boxes[i][3]) {
                    return i;
                }
            }
            return -1;
        };
        auto message_button_at = [&](int x, int y) {
            static const char* names[] = {
                "save", "load", "qsave", "qload", "auto", "voice", "skip", "config", "backlog", "close",
            };
            static const int boxes[][4] = {
                {413, 743, 38, 17}, {477, 744, 40, 15}, {543, 743, 54, 17},
                {622, 744, 56, 15}, {745, 743, 13, 17}, {785, 742, 21, 19},
                {833, 743, 16, 17}, {876, 742, 17, 19}, {920, 743, 15, 17},
                {962, 743, 15, 17},
            };
            for (int i = 0; i < 10; ++i) {
                if (x >= boxes[i][0] && y >= boxes[i][1] &&
                    x < boxes[i][0] + boxes[i][2] && y < boxes[i][1] + boxes[i][3]) {
                    (void)names;
                    return i;
                }
            }
            return -1;
        };
        bool running = true;
        auto title_activate = [&](int index) {
            if (index == 0) {
                start_new_game();
            } else if (index == 1) {
                const int latest = saves.latest_slot();
                if (latest == 0 || !load_slot(latest)) start_new_game();
            } else if (index == 2) {
                open_load_menu(Screen::Title);
            } else if (index == 3) {
                open_settings(Screen::Title);
            } else if (index == 4) {
                open_gallery(Screen::Title);
            } else {
                running = false;
            }
        };

        bool screenshot_done = false;
        if (!options.ui_screenshot.empty()) {
            if (options.ui_screen == "save") {
                refresh_slots();
                renderer.draw_save_menu(slots, 0, true);
            } else if (options.ui_screen == "load") {
                refresh_slots();
                renderer.draw_save_menu(slots, 0, false);
            } else if (options.ui_screen == "settings") {
                renderer.draw_settings(settings, 0, 1);
            } else if (options.ui_screen == "gallery") {
                renderer.draw_gallery(gallery_items, 0);
            } else {
                renderer.draw_title(std::vector<std::string>(6), 0);
            }
            if (!renderer.save_png(options.ui_screenshot)) {
                std::cerr << "[warn] UI screenshot save failed: " << options.ui_screenshot << "\n";
            }
            renderer.present();
            screenshot_done = true;
        }

        if (screen == Screen::Game && options.ui_screenshot.empty()) {
            engine.start();
            play_audio();
        }

        if (!options.screenshot.empty()) {
            for (int i = 0; i < options.steps; ++i) {
                engine.step(artc::Action::Next);
                play_audio();
            }
            renderer.draw(engine.snapshot());
            if (!renderer.save_png(options.screenshot)) {
                std::cerr << "[warn] screenshot save failed: " << options.screenshot << "\n";
            }
            renderer.present();
            screenshot_done = true;
        }

        const std::vector<std::string> title_items = {
            "はじめから", "つづきから", "ロード", "設定", "CG鑑賞", "終了",
        };
        const std::vector<std::string> pause_items = {
            "ゲームに戻る", "セーブ", "ロード", "設定", "CG鑑賞", "タイトルへ", "終了",
        };

        Uint32 last_skip = 0;
        while (running && !screenshot_done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                for (artc::Action action : input.handle_event(event)) {
                    if (action == artc::Action::Quit) {
                        running = false;
                        continue;
                    }
                    if (action == artc::Action::ToggleDebug) {
                        renderer.toggle_debug();
                        continue;
                    }

                    if (screen == Screen::Brand) {
                        const Uint32 elapsed = SDL_GetTicks() - brand_start;
                        const std::string ui = config.resolve_magic(":ui/");
                        const std::string cg = config.resolve_magic(":cg/");
                        if (elapsed < 1200) {
                            renderer.draw_brand("", true);
                            brand_stage = 0;
                        } else if (elapsed < 3500) {
                            if (!brand_voice_played) {
                                artc::AudioCommand voice;
                                voice.kind = artc::AudioKind::Bgm;
                                voice.path = "sound/ogg/sysse/vo/sys_aki_brand.ogg";
                                voice.channel = 5;
                                audio.play({voice});
                                brand_voice_played = true;
                            }
                            renderer.draw_brand(cg + "logo", true);
                            brand_stage = 1;
                        } else if (elapsed < 5000) {
                            renderer.draw_brand(ui + "logo02", true);
                            brand_stage = 2;
                        } else if (elapsed < 6500) {
                            renderer.draw_brand(ui + "logo01", true);
                            brand_stage = 3;
                        } else if (elapsed < 8500) {
                            renderer.draw_brand(ui + "sys_warning_bg", false);
                            brand_stage = 4;
                        } else {
                            screen = Screen::Title;
                            title_cursor = 0;
                            play_title_bgm();
                        }
                        if (screen == Screen::Brand) {
                            audio.update();
                            renderer.present();
                            SDL_Delay(16);
                        }
                        continue;
                    }

                    if (screen == Screen::Title) {
                        if (action == artc::Action::Menu) {
                            running = false;
                        } else if (action == artc::Action::Pointer) {
                            int x = 0;
                            int y = 0;
                            if (pointer_logical(x, y)) {
                                const int hit = title_button_at(x, y);
                                if (hit >= 0) {
                                    static const int boxes[][4] = {
                                        {736, 329, 150, 48}, {736, 393, 207, 48},
                                        {736, 456, 137, 49}, {736, 520, 168, 48},
                                        {736, 583, 177, 48}, {687, 647, 302, 94},
                                    };
                                    renderer.begin_press(boxes[hit][0], boxes[hit][1],
                                                         boxes[hit][2], boxes[hit][3]);
                                    title_cursor = hit;
                                    pending_title_action = hit;
                                    pending_title_at = SDL_GetTicks() + 140;
                                }
                            }
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            title_cursor = (title_cursor + 5) % 6;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            title_cursor = (title_cursor + 1) % 6;
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            title_activate(title_cursor);
                        }
                        continue;
                    }

                    if (screen == Screen::Game) {
                        if (action == artc::Action::Menu) {
                            pause_cursor = 0;
                            screen = Screen::Pause;
                        } else if (action == artc::Action::QuickSave) {
                            open_save_menu(Screen::Game);
                        } else if (action == artc::Action::QuickLoad) {
                            open_load_menu(Screen::Game);
                        } else if (action == artc::Action::Back) {
                            restore_history();
                        } else if (action == artc::Action::Pointer) {
                            int px = 0;
                            int py = 0;
                            const int hit = pointer_logical(px, py) ? message_button_at(px, py) : -1;
                            if (hit == 0) {
                                open_save_menu(Screen::Game);
                            } else if (hit == 1) {
                                open_load_menu(Screen::Game);
                            } else if (hit == 2) {
                                open_save_menu(Screen::Game);
                            } else if (hit == 3) {
                                open_load_menu(Screen::Game);
                            } else if (hit == 7) {
                                open_settings(Screen::Game);
                            } else if (hit == 9) {
                                engine.step(artc::Action::ToggleUi);
                            } else if (hit == 8) {
                                restore_history();
                            } else if (hit == 6) {
                                engine.step(artc::Action::Skip);
                                play_audio();
                            } else if (hit >= 0) {
                                // auto/voice buttons are decorative until their systems exist
                            } else {
                                advance_text();
                            }
                        } else {
                            if (action == artc::Action::Next ||
                                action == artc::Action::Confirm ||
                                action == artc::Action::Skip) {
                                advance_text();
                            } else {
                                engine.step(action);
                                play_audio();
                            }
                        }
                        continue;
                    }

                    if (screen == Screen::Pause) {
                        if (action == artc::Action::Menu) {
                            screen = Screen::Game;
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            pause_cursor = (pause_cursor + 6) % 7;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            pause_cursor = (pause_cursor + 1) % 7;
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            if (pause_cursor == 0) {
                                screen = Screen::Game;
                            } else if (pause_cursor == 1) {
                                open_save_menu(Screen::Pause);
                            } else if (pause_cursor == 2) {
                                open_load_menu(Screen::Pause);
                            } else if (pause_cursor == 3) {
                                open_settings(Screen::Pause);
                            } else if (pause_cursor == 4) {
                                open_gallery(Screen::Pause);
                            } else if (pause_cursor == 5) {
                                stop_game_audio();
                                screen = Screen::Title;
                                title_cursor = 0;
                            } else {
                                running = false;
                            }
                        }
                        continue;
                    }

                    if (screen == Screen::Settings) {
                        const int item_count = settings_page == 1 ? 8 : (settings_page == 2 ? 6 : 1);
                        if (action == artc::Action::Menu || action == artc::Action::Back) {
                            save_current_settings();
                            screen = menu_return;
                        } else if (action == artc::Action::PageLeft) {
                            settings_page = std::max(1, settings_page - 1);
                            settings_cursor = 0;
                        } else if (action == artc::Action::PageRight) {
                            settings_page = std::min(3, settings_page + 1);
                            settings_cursor = 0;
                        } else if (action == artc::Action::Pointer) {
                            int x = 0;
                            int y = 0;
                            if (pointer_logical(x, y)) {
                                if (x >= 213 && x < 420 && y >= 60 && y < 108) {
                                    settings_page = 1;
                                    settings_cursor = 0;
                                } else if (x >= 421 && x < 624 && y >= 60 && y < 108) {
                                    settings_page = 2;
                                    settings_cursor = 0;
                                } else if (x >= 625 && x < 815 && y >= 60 && y < 108) {
                                    settings_page = 3;
                                    settings_cursor = 0;
                                } else if (x >= 484 && x < 574 && y >= 726 && y < 760) {
                                    settings = artc::UserSettings{};
                                    save_current_settings();
                                } else if (x >= 886 && x < 965 && y >= 727 && y < 760) {
                                    save_current_settings();
                                    screen = menu_return;
                                } else if (x >= 733 && x < 833 && y >= 727 && y < 760) {
                                    save_current_settings();
                                    screen = Screen::Title;
                                } else if (settings_page == 1) {
                                    if (y >= 130 && y < 165) {
                                        settings.fullscreen = x >= 400;
                                        settings_cursor = 0;
                                    } else if (y >= 195 && y < 235) {
                                        settings.effect = x < 400;
                                        settings_cursor = 1;
                                    } else if (y >= 265 && y < 305) {
                                        settings.sysse_volume = std::clamp((x - 269) / 228.0f, 0.0f, 1.0f);
                                        settings_cursor = 2;
                                    } else if (y >= 360 && y < 400 && x < 700) {
                                        settings.text_speed = std::clamp((x - 196) / 228.0f, 0.0f, 1.0f) * 3.0f;
                                        settings_cursor = 3;
                                    } else if (y >= 435 && y < 475 && x < 700) {
                                        settings.auto_speed = std::clamp((x - 196) / 228.0f, 0.0f, 1.0f) * 3.0f;
                                        settings_cursor = 4;
                                    } else if (y >= 510 && y < 550) {
                                        settings.window_alpha = std::clamp((x - 196) / 228.0f, 0.0f, 1.0f);
                                        settings_cursor = 5;
                                    } else if (y >= 355 && y < 390 && x >= 700) {
                                        settings.text_color = x < 850;
                                        settings_cursor = 6;
                                    } else if (y >= 430 && y < 465 && x >= 700) {
                                        settings.skip_unread = x < 850;
                                        settings_cursor = 7;
                                    }
                                } else if (settings_page == 2) {
                                    const int row = (y - 190) / 72;
                                    if (y >= 190 && y < 630 && x >= 196 && x < 424) {
                                        const float value = std::clamp((x - 196) / 228.0f, 0.0f, 1.0f);
                                        settings_cursor = std::clamp(row, 0, 4);
                                        if (row == 0) settings.master_volume = value;
                                        else if (row == 1) settings.bgm_volume = value;
                                        else if (row == 2) settings.se_volume = value;
                                        else if (row == 3) settings.voice_volume = value;
                                        else if (row == 4) settings.sysse_volume = value;
                                    }
                                }
                                save_current_settings();
                            }
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            settings_cursor = (settings_cursor + item_count - 1) % item_count;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            settings_cursor = (settings_cursor + 1) % item_count;
                        } else if (action == artc::Action::Left ||
                                   action == artc::Action::Right ||
                                   action == artc::Action::Confirm ||
                                   action == artc::Action::Next) {
                            const float direction =
                                action == artc::Action::Left ? -1.0f : 1.0f;
                            const bool activate = action == artc::Action::Confirm ||
                                                  action == artc::Action::Next;
                            if (settings_page == 1) {
                                if (settings_cursor == 0) {
                                    if (activate || action == artc::Action::Left ||
                                        action == artc::Action::Right) settings.fullscreen = !settings.fullscreen;
                                } else if (settings_cursor == 1) {
                                    if (activate || action == artc::Action::Left ||
                                        action == artc::Action::Right) settings.effect = !settings.effect;
                                } else if (settings_cursor == 2) {
                                    settings.sysse_volume += direction * 0.05f;
                                } else if (settings_cursor == 3) {
                                    settings.text_speed += direction * 0.1f;
                                } else if (settings_cursor == 4) {
                                    settings.auto_speed += direction * 0.1f;
                                } else if (settings_cursor == 5) {
                                    settings.window_alpha += direction * 0.05f;
                                } else if (settings_cursor == 6) {
                                    if (activate || action == artc::Action::Left ||
                                        action == artc::Action::Right) settings.text_color = !settings.text_color;
                                } else if (settings_cursor == 7) {
                                    if (activate || action == artc::Action::Left ||
                                        action == artc::Action::Right) settings.skip_unread = !settings.skip_unread;
                                }
                            } else if (settings_page == 2) {
                                if (settings_cursor == 0) settings.master_volume += direction * 0.05f;
                                else if (settings_cursor == 1) settings.bgm_volume += direction * 0.05f;
                                else if (settings_cursor == 2) settings.se_volume += direction * 0.05f;
                                else if (settings_cursor == 3) settings.voice_volume += direction * 0.05f;
                                else if (settings_cursor == 4) settings.sysse_volume += direction * 0.05f;
                                else if (settings_cursor == 5) settings.bgm_voice_fade = !settings.bgm_voice_fade;
                            }
                            save_current_settings();
                        }
                        continue;
                    }

                    if (screen == Screen::Gallery) {
                        if (action == artc::Action::Menu || action == artc::Action::Back) {
                            screen = menu_return;
                        } else if (action == artc::Action::Pointer) {
                            int x = 0;
                            int y = 0;
                            if (pointer_logical(x, y)) {
                                if (x >= 751 && x < 860 && y >= 71 && y < 104) {
                                    screen = menu_return;
                                } else {
                                    for (int i = 0; i < static_cast<int>(gallery_items.size()) && i < 20; ++i) {
                                        const int cx = 30 + (i % 4) * 245;
                                        const int cy = 123 + (i / 4) * 127;
                                        if (x >= cx && y >= cy && x < cx + 230 && y < cy + 119) {
                                            gallery_cursor = i;
                                            if (gallery_items[i].unlocked) {
                                                cg_view_path = gallery_items[i].full;
                                                screen = Screen::CGView;
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            if (!gallery_items.empty()) {
                                gallery_cursor = (gallery_cursor + static_cast<int>(gallery_items.size()) - 4) %
                                                 static_cast<int>(gallery_items.size());
                            }
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            if (!gallery_items.empty()) {
                                gallery_cursor = (gallery_cursor + 4) %
                                                 static_cast<int>(gallery_items.size());
                            }
                        } else if (action == artc::Action::Left) {
                            if (!gallery_items.empty()) {
                                gallery_cursor = (gallery_cursor + static_cast<int>(gallery_items.size()) - 1) %
                                                 static_cast<int>(gallery_items.size());
                            }
                        } else if (action == artc::Action::Right) {
                            if (!gallery_items.empty()) {
                                gallery_cursor = (gallery_cursor + 1) %
                                                 static_cast<int>(gallery_items.size());
                            }
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            if (!gallery_items.empty() && gallery_items[gallery_cursor].unlocked) {
                                cg_view_path = gallery_items[gallery_cursor].full;
                                screen = Screen::CGView;
                            }
                        }
                        continue;
                    }

                    if (screen == Screen::CGView) {
                        if (action == artc::Action::Menu || action == artc::Action::Back ||
                            action == artc::Action::Next || action == artc::Action::Confirm) {
                            screen = Screen::Gallery;
                        }
                        continue;
                    }

                    if (screen == Screen::Save || screen == Screen::Load) {
                        if (action == artc::Action::Menu || action == artc::Action::Back) {
                            screen = menu_return;
                        } else if (action == artc::Action::Pointer) {
                            int x = 0;
                            int y = 0;
                            if (pointer_logical(x, y)) {
                                bool handled = false;
                                for (int i = 0; i < 12 && i < static_cast<int>(slots.size()); ++i) {
                                    const int cx = 36 + (i % 3) * 319;
                                    const int cy = 145 + (i / 3) * 136;
                                    if (x >= cx && y >= cy && x < cx + 316 && y < cy + 132) {
                                        slot_cursor = i;
                                        handled = true;
                                        break;
                                    }
                                }
                                if (!handled && x >= 886 && x < 965 && y >= 727 && y < 760) {
                                    screen = menu_return;
                                } else if (!handled && save_mode &&
                                           x >= 516 && x < 572 && y >= 726 && y < 760) {
                                    save_selected();
                                }
                            }
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            slot_cursor = (slot_cursor + artc::SaveManager::kSlotCount - 1) %
                                          artc::SaveManager::kSlotCount;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            slot_cursor = (slot_cursor + 1) % artc::SaveManager::kSlotCount;
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            if (save_mode) {
                                save_selected();
                            } else if (!slots.empty()) {
                                load_slot(slots[slot_cursor].slot);
                            }
                        }
                        continue;
                    }
                }
            }
            if (!running) break;

            if (pending_title_action >= 0 && SDL_GetTicks() >= pending_title_at) {
                const int action = pending_title_action;
                pending_title_action = -1;
                title_activate(action);
            }

            if (screen != previous_screen) {
                previous_screen = screen;
                screen_changed_ms = SDL_GetTicks();
            }
            {
                const float age = static_cast<float>(SDL_GetTicks() - screen_changed_ms);
                renderer.set_fade(std::max(0.0f, 1.0f - age / 220.0f));
            }

            if (screen == Screen::Brand) {
                const Uint32 elapsed = SDL_GetTicks() - brand_start;
                const std::string ui = config.resolve_magic(":ui/");
                const std::string cg = config.resolve_magic(":cg/");
                if (elapsed < 1200) {
                    renderer.draw_brand("", true);
                    brand_stage = 0;
                } else if (elapsed < 3500) {
                    if (!brand_voice_played) {
                        artc::AudioCommand voice;
                        voice.kind = artc::AudioKind::Bgm;
                        voice.path = "sound/ogg/sysse/vo/sys_aki_brand.ogg";
                        voice.channel = 5;
                        audio.play({voice});
                        brand_voice_played = true;
                    }
                    renderer.draw_brand(cg + "logo", true);
                    brand_stage = 1;
                } else if (elapsed < 5000) {
                    renderer.draw_brand(ui + "logo02", true);
                    brand_stage = 2;
                } else if (elapsed < 6500) {
                    renderer.draw_brand(ui + "logo01", true);
                    brand_stage = 3;
                } else if (elapsed < 8500) {
                    renderer.draw_brand(ui + "sys_warning_bg", false);
                    brand_stage = 4;
                } else {
                    screen = Screen::Title;
                    title_cursor = 0;
                    play_title_bgm();
                }
                audio.update();
                if (screen == Screen::Brand) renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::Game) {
                if (input.skip_held()) {
                    const Uint32 now = SDL_GetTicks();
                    if (now - last_skip >= 80) {
                        advance_text();
                        last_skip = now;
                    }
                }
                const std::string& event_asset = engine.snapshot().background;
                if (event_asset != last_event_asset &&
                    event_asset.find("/ev/") != std::string::npos) {
                    last_event_asset = event_asset;
                    gallery.unlock_for_asset(event_asset);
                    refresh_gallery();
                    gallery.save(options.save_dir / "gallery.json");
                }
                const std::string& text = engine.snapshot().text;
                if (text != last_revealed_text) {
                    last_revealed_text = text;
                    reveal_chars = 0;
                    reveal_tick = SDL_GetTicks();
                }
                const std::size_t total_chars = utf8_length(text);
                if (reveal_chars < total_chars) {
                    const Uint32 now = SDL_GetTicks();
                    const Uint32 elapsed = now - reveal_tick;
                    if (elapsed > 0) {
                        const std::size_t add = static_cast<std::size_t>(
                            elapsed * 0.045f * settings.text_speed);
                        reveal_chars = std::min(total_chars, reveal_chars + std::max<std::size_t>(1, add));
                        reveal_tick = now;
                    }
                }
                renderer.set_text_reveal(reveal_chars);
                audio.update();
                renderer.draw(engine.snapshot());
                renderer.present();
                SDL_Delay(engine.finished() ? 16 : 8);
            } else if (screen == Screen::Title) {
                audio.update();
                renderer.draw_title(title_items, title_cursor, saves.latest_slot() != 0);
                renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::Pause) {
                audio.update();
                renderer.draw_pause_menu(pause_items, pause_cursor);
                renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::Settings) {
                audio.update();
                renderer.draw_settings(settings, settings_cursor, settings_page);
                renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::Gallery) {
                audio.update();
                renderer.draw_gallery(gallery_items, gallery_cursor);
                renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::CGView) {
                audio.update();
                renderer.draw_cg_view(cg_view_path);
                renderer.present();
                SDL_Delay(16);
            } else {
                audio.update();
                renderer.draw_save_menu(slots, slot_cursor, save_mode);
                renderer.present();
                SDL_Delay(16);
            }
        }
        audio.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        tlog(std::string("[fatal] ") + e.what());
#ifdef __SWITCH__
        if (debug_log) {
            debug_log << "[fatal] " << e.what() << "\n";
            debug_log.flush();
        }
#endif
        exit_code = 2;
    }
    if (window) SDL_DestroyWindow(window);

    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
#ifdef __SWITCH__
    if (romfs_mounted) romfsExit();
#endif
    return exit_code;
}
