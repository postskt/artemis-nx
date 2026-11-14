#include "InputHandler.hpp"

#include <algorithm>
#include <cstdio>

namespace artc {

InputHandler::InputHandler() {
    SDL_GameControllerEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) open_controller(i);
    }
}

InputHandler::~InputHandler() {
    for (auto* controller : controllers_) {
        if (controller) SDL_GameControllerClose(controller);
    }
    controllers_.clear();
}

void InputHandler::open_controller(int device_index) {
    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (!controller) {
        std::fprintf(stderr, "[input] cannot open controller %d: %s\n",
                     device_index, SDL_GetError());
        return;
    }
    controllers_.push_back(controller);
    const char* name = SDL_GameControllerName(controller);
    std::fprintf(stderr, "[input] controller: %s\n", name ? name : "unknown");
}

void InputHandler::close_controller(SDL_JoystickID instance_id) {
    auto it = std::find_if(controllers_.begin(), controllers_.end(),
                           [&](SDL_GameController* controller) {
                               SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
                               return joystick && SDL_JoystickInstanceID(joystick) == instance_id;
                           });
    if (it != controllers_.end()) {
        SDL_GameControllerClose(*it);
        controllers_.erase(it);
    }
}

std::vector<Action> InputHandler::handle_event(const SDL_Event& event) {
    std::vector<Action> actions;
    switch (event.type) {
        case SDL_QUIT:
            actions.push_back(Action::Quit);
            break;
        case SDL_KEYDOWN:
            if (event.key.repeat) break;
            switch (event.key.keysym.sym) {
                case SDLK_RETURN:
                case SDLK_SPACE:
                case SDLK_z:
                    actions.push_back(Action::Next);
                    break;
                case SDLK_b:
                case SDLK_BACKSPACE:
                    actions.push_back(Action::ToggleUi);
                    break;
                case SDLK_ESCAPE:
                    actions.push_back(Action::Quit);
                    break;
                case SDLK_F1:
                    actions.push_back(Action::ToggleDebug);
                    break;
                case SDLK_UP:
                    actions.push_back(Action::SelectUp);
                    break;
                case SDLK_DOWN:
                    actions.push_back(Action::SelectDown);
                    break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:
                    actions.push_back(Action::Skip);
                    break;
                default:
                    break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) actions.push_back(Action::Next);
            if (event.button.button == SDL_BUTTON_RIGHT) actions.push_back(Action::ToggleUi);
            break;
        case SDL_FINGERDOWN:
            actions.push_back(Action::Next);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                case SDL_CONTROLLER_BUTTON_B:
                    actions.push_back(Action::Next);
                    break;
                case SDL_CONTROLLER_BUTTON_BACK:
                    actions.push_back(Action::ToggleUi);
                    break;
                case SDL_CONTROLLER_BUTTON_START:
                    actions.push_back(Action::ToggleDebug);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    actions.push_back(Action::SelectUp);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    actions.push_back(Action::SelectDown);
                    break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    actions.push_back(Action::Skip);
                    break;
                default:
                    break;
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                if (event.caxis.value < -16000 && !axis_up_latched_) {
                    actions.push_back(Action::SelectUp);
                    axis_up_latched_ = true;
                } else if (event.caxis.value > 16000 && !axis_down_latched_) {
                    actions.push_back(Action::SelectDown);
                    axis_down_latched_ = true;
                } else if (event.caxis.value > -8000 && event.caxis.value < 8000) {
                    axis_up_latched_ = false;
                    axis_down_latched_ = false;
                }
            }
            break;
        default:
            break;
    }
    return actions;
}

bool InputHandler::skip_held() const {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys && (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])) return true;
    for (auto* controller : controllers_) {
        if (controller &&
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            return true;
        }
    }
    return false;
}

}  // namespace artc
