#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "AssetManager.hpp"
#include "Config.hpp"
#include "Engine.hpp"
#include "LuaLoader.hpp"

int main(int argc, char** argv) {
    std::filesystem::path root = "build_temp/pc_unpacked";
    std::filesystem::path config_path = "cpp_runner/runner_config.json";
    int max_steps = 20;
    bool parse_all = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--max-steps" && i + 1 < argc) {
            max_steps = std::atoi(argv[++i]);
        } else if (arg == "--parse-all") {
            parse_all = true;
        }
    }

    try {
        artc::Config config = artc::Config::load(config_path);
        artc::AssetManager assets(config, root);
        artc::LuaLoader loader;
        if (parse_all) {
            std::size_t ok = 0;
            std::size_t blocks = 0;
            std::size_t texts = 0;
            for (const auto& named : config.scripts()) {
                auto actual = assets.resolve(named);
                if (!actual) continue;
                try {
                    artc::AstScript script = loader.load_ast(assets.root() / *actual);
                    ++ok;
                    blocks += script.blocks.size();
                    texts += script.texts.size();
                } catch (const std::exception& e) {
                    std::cerr << "parse failed: " << named << ": " << e.what() << "\n";
                }
            }
            std::cout << "parse-all: scripts=" << ok << " blocks=" << blocks
                      << " texts=" << texts << " warnings=" << assets.warnings().size() << "\n";
            return ok == config.scripts().size() ? 0 : 1;
        }
        artc::Engine engine(config, assets, loader);
        engine.start();
        std::cout << "start: script=" << engine.snapshot().script_name
                  << " block=" << engine.snapshot().block_index
                  << " text=" << engine.snapshot().text << "\n";

        int steps = 0;
        while (!engine.finished() && steps < max_steps) {
            const auto audio = engine.consume_audio();
            if (!audio.empty()) {
                std::cout << "audio: " << audio.size() << " cue(s)\n";
            }
            engine.step(artc::Action::Next);
            const auto& state = engine.snapshot();
            if (!state.text.empty()) {
                std::cout << "[" << state.script_name << ":" << state.block_index
                          << "] " << (state.speaker.empty() ? "" : state.speaker + " ")
                          << state.text.substr(0, 60) << "\n";
            }
            std::cout << "  bg=" << state.background
                      << " fg=" << state.active_fgs.size()
                      << " f=" << state.local_vars.size()
                      << " g=" << state.global_vars.size() << "\n";
            ++steps;
        }
        const auto& final_state = engine.snapshot();
        std::cout << "done: steps=" << steps
                  << " finished=" << (engine.finished() ? 1 : 0)
                  << " script=" << final_state.script_name
                  << " block=" << final_state.block_index
                  << " warnings=" << assets.warnings().size() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "core smoke error: " << e.what() << "\n";
        return 1;
    }
}
