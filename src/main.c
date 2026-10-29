// Phase 0 entry point: verify the user's ROM, then open an empty window.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

#include "rom.h"

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MK64 Port", msg, NULL);
}

int main(int argc, char **argv) {
    // ROM path: first argument, or "rom.z64" next to the executable.
    char default_path[1024];
    const char *base = SDL_GetBasePath();
    SDL_snprintf(default_path, sizeof(default_path), "%srom.z64", base ? base : "");
    const char *rom_path = argc > 1 ? argv[1] : default_path;

    Rom rom;
    RomStatus status = rom_load(rom_path, &rom);
    if (status != ROM_OK) {
        char msg[1024];
        SDL_snprintf(msg, sizeof(msg), "%s\n\nArquivo: %s%s%s", rom_status_message(status), rom_path,
                     rom.data ? "\nSHA-1: " : "", rom.data ? rom.sha1 : "");
        fail(msg);
        rom_free(&rom);
        return 1;
    }
    printf("%s\n  titulo: %s  codigo: %s  sha1: %s\n", rom_status_message(status), rom.title,
           rom.game_code, rom.sha1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fail(SDL_GetError());
        rom_free(&rom);
        return 1;
    }
    char title[128];
    SDL_snprintf(title, sizeof(title), "MK64 Port - %s (%s)", rom.title, rom.game_code);
    SDL_Window *window;
    SDL_Renderer *renderer;
    if (!SDL_CreateWindowAndRenderer(title, 640, 480, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fail(SDL_GetError());
        SDL_Quit();
        rom_free(&rom);
        return 1;
    }
    // Draw at the N64's 320x240 and let SDL scale it.
    SDL_SetRenderLogicalPresentation(renderer, 320, 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                running = 0;
            }
        }
        SDL_SetRenderDrawColor(renderer, 16, 16, 32, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        SDL_RenderDebugText(renderer, 8, 8, "MK64 Port - Fase 0");
        SDL_RenderDebugText(renderer, 8, 24, rom.title);
        SDL_RenderDebugText(renderer, 8, 40, "ROM verificada (SHA-1 ok)");
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    rom_free(&rom);
    return 0;
}
