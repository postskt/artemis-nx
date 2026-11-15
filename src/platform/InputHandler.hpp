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
    int pointer_x() const { return pointer_x_; }
    int pointer_y() const { return pointer_y_; }

private:
    std::vector<SDL_GameController*> controllers_;
    bool axis_up_latched_ = false;
    bool axis_down_latched_ = false;
    int pointer_x_ = 0;
    int pointer_y_ = 0;

    void open_controller(int device_index);
    void close_controller(SDL_JoystickID instance_id);
};

}  // namespace artc
