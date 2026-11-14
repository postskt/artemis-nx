#pragma once

#include <vector>

#include <SDL.h>

#include "Engine.hpp"

namespace artc {

class InputHandler {
public:
    InputHandler();
    ~InputHandler();

    std::vector<Action> handle_event(const SDL_Event& event);
    bool skip_held() const;

private:
    std::vector<SDL_GameController*> controllers_;
    bool axis_up_latched_ = false;
    bool axis_down_latched_ = false;

    void open_controller(int device_index);
    void close_controller(SDL_JoystickID instance_id);
};

}  // namespace artc
