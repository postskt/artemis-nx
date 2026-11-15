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

std::vector<Action> InputHandler::poll() {
    std::vector<Action> actions;
#ifdef __SWITCH__
    if (!pad_initialized_) {
        padInitializeDefault(&pad_);
        pad_initialized_ = true;
    }
    padUpdate(&pad_);
    const u64 down = padGetButtonsDown(&pad_);
    if (down & HidNpadButton_A) actions.push_back(Action::Confirm);
    if (down & HidNpadButton_B) actions.push_back(Action::ToggleUi);
    if (down & HidNpadButton_X) actions.push_back(Action::QuickSave);
    if (down & HidNpadButton_Y) actions.push_back(Action::QuickLoad);
    if (down & HidNpadButton_Plus) actions.push_back(Action::Menu);
    if (down & HidNpadButton_Minus) actions.push_back(Action::ToggleUi);
    if (down & HidNpadButton_AnyUp) actions.push_back(Action::SelectUp);
    if (down & HidNpadButton_AnyDown) actions.push_back(Action::SelectDown);
    if (down & HidNpadButton_AnyLeft) actions.push_back(Action::Left);
    if (down & HidNpadButton_AnyRight) actions.push_back(Action::Right);
    if (down & HidNpadButton_L) actions.push_back(Action::PageLeft);
    if (down & HidNpadButton_R) actions.push_back(Action::PageRight);
#endif
    if (!actions.empty()) {
        pending_.insert(pending_.end(), actions.begin(), actions.end());
        SDL_Event event{};
        event.type = SDL_USEREVENT;
        SDL_PushEvent(&event);
    }
    return actions;
}

std::vector<Action> InputHandler::handle_event(const SDL_Event& event) {
    if (!pending_.empty()) {
        std::vector<Action> actions(pending_.begin(), pending_.end());
        pending_.clear();
        return actions;
    }
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
                    actions.push_back(Action::ToggleUi);
                    break;
                case SDLK_BACKSPACE:
                    actions.push_back(Action::Back);
                    break;
                case SDLK_ESCAPE:
                    actions.push_back(Action::Menu);
                    break;
                case SDLK_F1:
                    actions.push_back(Action::ToggleDebug);
                    break;
                case SDLK_F5:
                    actions.push_back(Action::QuickSave);
                    break;
                case SDLK_F9:
                    actions.push_back(Action::QuickLoad);
                    break;
                case SDLK_UP:
                    actions.push_back(Action::SelectUp);
                    break;
                case SDLK_DOWN:
                    actions.push_back(Action::SelectDown);
                    break;
                case SDLK_LEFT:
                    actions.push_back(Action::Left);
                    break;
                case SDLK_RIGHT:
                    actions.push_back(Action::Right);
                    break;
                case SDLK_q:
                    actions.push_back(Action::PageLeft);
                    break;
                case SDLK_e:
                    actions.push_back(Action::PageRight);
                    break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:
                    actions.push_back(Action::Skip);
                    break;
                default:
                    break;
            }
            break;
        case SDL_MOUSEMOTION:
            pointer_x_ = event.motion.x;
            pointer_y_ = event.motion.y;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                pointer_x_ = event.button.x;
                pointer_y_ = event.button.y;
                actions.push_back(Action::Pointer);
            }
            if (event.button.button == SDL_BUTTON_RIGHT) actions.push_back(Action::ToggleUi);
            break;
        case SDL_FINGERDOWN: {
            int window_width = 1024;
            int window_height = 768;
            SDL_Window* window = SDL_GetWindowFromID(event.tfinger.windowID);
            if (window) SDL_GetWindowSize(window, &window_width, &window_height);
            pointer_x_ = static_cast<int>(event.tfinger.x * window_width);
            pointer_y_ = static_cast<int>(event.tfinger.y * window_height);
            actions.push_back(Action::Pointer);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            open_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    actions.push_back(Action::Confirm);
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    actions.push_back(Action::ToggleUi);
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    actions.push_back(Action::QuickSave);
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    actions.push_back(Action::QuickLoad);
                    break;
                case SDL_CONTROLLER_BUTTON_BACK:
                    actions.push_back(Action::Menu);
                    break;
                case SDL_CONTROLLER_BUTTON_START:
                    actions.push_back(Action::Menu);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    actions.push_back(Action::SelectUp);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    actions.push_back(Action::SelectDown);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    actions.push_back(Action::Left);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    actions.push_back(Action::Right);
                    break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                    actions.push_back(Action::PageLeft);
                    break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    actions.push_back(Action::PageRight);
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
#ifdef __SWITCH__
    if (pad_initialized_ &&
        (padGetButtons(&pad_) & (HidNpadButton_ZR | HidNpadButton_R))) {
        return true;
    }
#endif
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys && (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])) return true;
    for (auto* controller : controllers_) {
        if (controller &&
            (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
             SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)) {
            return true;
        }
    }
    return false;
}

}  // namespace artc
