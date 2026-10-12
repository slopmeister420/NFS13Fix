// standalone check of the bundled sdl: lists what it sees for a few seconds
#include <SDL3/SDL.h>
#include <cstdio>

int main()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // same overrides as the plugin
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "0",
                            SDL_HINT_OVERRIDE);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("init failed: %s\n", SDL_GetError());
        return 1;
    }
    for (int t = 0; t < 400; ++t) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_JOYSTICK_ADDED)
                printf("joystick added %u: %s gamepad=%d\n", e.jdevice.which,
                       SDL_GetJoystickNameForID(e.jdevice.which), SDL_IsGamepad(e.jdevice.which));
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which);
                printf("gamepad added %u: %s %s\n", e.gdevice.which, SDL_GetGamepadName(g),
                       g ? "" : SDL_GetError());
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                printf("button %d\n", e.gbutton.button);
        }
        SDL_Delay(10);
    }
    int n = 0;
    SDL_free(SDL_GetJoysticks(&n));
    printf("joysticks at end: %d\n", n);
    SDL_Quit();
    return 0;
}
