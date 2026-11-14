#include <cstdlib>
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
#include "GameSelector.hpp"
#include "InputHandler.hpp"
#include "LuaLoader.hpp"
#include "Renderer.hpp"
#include "SaveManager.hpp"

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

        enum class Screen { Title, Game, Save, Load, Pause };
        Screen screen = (options.no_title || !options.screenshot.empty())
                            ? Screen::Game
                            : Screen::Title;
        Screen menu_return = Screen::Game;
        int title_cursor = 0;
        int pause_cursor = 0;
        int slot_cursor = 0;
        bool save_mode = false;
        std::string pending_thumbnail;
        std::vector<artc::SaveSlotInfo> slots;

        auto refresh_slots = [&] {
            const auto list = saves.list();
            slots.assign(list.begin(), list.end());
        };
        auto play_audio = [&] {
            audio.play(engine.consume_audio());
        };
        auto start_new_game = [&] {
            engine.start();
            play_audio();
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
            play_audio();
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

        if (screen == Screen::Game) {
            engine.start();
            play_audio();
        }

        bool screenshot_done = false;
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
            "はじめから", "つづきから", "ロード", "終了",
        };
        const std::vector<std::string> pause_items = {
            "ゲームに戻る", "セーブ", "ロード", "タイトルへ", "終了",
        };

        bool running = true;
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

                    if (screen == Screen::Title) {
                        if (action == artc::Action::Menu) {
                            running = false;
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            title_cursor = (title_cursor + 3) % 4;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            title_cursor = (title_cursor + 1) % 4;
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            if (title_cursor == 0) {
                                start_new_game();
                            } else if (title_cursor == 1) {
                                const int latest = saves.latest_slot();
                                if (latest == 0 || !load_slot(latest)) start_new_game();
                            } else if (title_cursor == 2) {
                                open_load_menu(Screen::Title);
                            } else {
                                running = false;
                            }
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
                        } else {
                            engine.step(action);
                            play_audio();
                        }
                        continue;
                    }

                    if (screen == Screen::Pause) {
                        if (action == artc::Action::Menu) {
                            screen = Screen::Game;
                        } else if (action == artc::Action::Up ||
                                   action == artc::Action::SelectUp) {
                            pause_cursor = (pause_cursor + 4) % 5;
                        } else if (action == artc::Action::Down ||
                                   action == artc::Action::SelectDown) {
                            pause_cursor = (pause_cursor + 1) % 5;
                        } else if (action == artc::Action::Next ||
                                   action == artc::Action::Confirm) {
                            if (pause_cursor == 0) {
                                screen = Screen::Game;
                            } else if (pause_cursor == 1) {
                                open_save_menu(Screen::Pause);
                            } else if (pause_cursor == 2) {
                                open_load_menu(Screen::Pause);
                            } else if (pause_cursor == 3) {
                                stop_game_audio();
                                screen = Screen::Title;
                                title_cursor = 0;
                            } else {
                                running = false;
                            }
                        }
                        continue;
                    }

                    if (screen == Screen::Save || screen == Screen::Load) {
                        if (action == artc::Action::Menu) {
                            screen = menu_return;
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

            if (screen == Screen::Game) {
                if (input.skip_held()) {
                    const Uint32 now = SDL_GetTicks();
                    if (now - last_skip >= 80) {
                        engine.step(artc::Action::Skip);
                        play_audio();
                        last_skip = now;
                    }
                }
                audio.update();
                renderer.draw(engine.snapshot());
                renderer.present();
                SDL_Delay(engine.finished() ? 16 : 8);
            } else if (screen == Screen::Title) {
                audio.update();
                renderer.draw_title(title_items, title_cursor);
                renderer.present();
                SDL_Delay(16);
            } else if (screen == Screen::Pause) {
                audio.update();
                renderer.draw_title(pause_items, pause_cursor);
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
