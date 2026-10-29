// Entry point: verify the user's ROM, open the window and run the recompiled game.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#include "input.h"
#include "rom.h"
#include "ultra.h"

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MK64 Port", msg, NULL);
}

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Rom rom;
} App;

// Until the graphics phase, each retrace draws a status screen of what the game is doing.
static int on_retrace(void *user) {
    App *app = user;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) return 0;
    }
    input_update();
    char line[128];
    SDL_SetRenderDrawColor(app->renderer, 16, 16, 32, 255);
    SDL_RenderClear(app->renderer);
    SDL_SetRenderDrawColor(app->renderer, 230, 230, 230, 255);
    SDL_RenderDebugText(app->renderer, 8, 8, "MK64 Port - runtime (Fase 4)");
    SDL_snprintf(line, sizeof line, "retraces: %llu", (unsigned long long)ultra_stats.retraces);
    SDL_RenderDebugText(app->renderer, 8, 32, line);
    SDL_snprintf(line, sizeof line, "tarefas RSP: gfx %llu audio %llu",
                 (unsigned long long)ultra_stats.gfx_tasks, (unsigned long long)ultra_stats.audio_tasks);
    SDL_RenderDebugText(app->renderer, 8, 48, line);
    SDL_snprintf(line, sizeof line, "DMA: %llu (%llu KB)", (unsigned long long)ultra_stats.dma_count,
                 (unsigned long long)(ultra_stats.dma_bytes / 1024));
    SDL_RenderDebugText(app->renderer, 8, 64, line);
    SDL_snprintf(line, sizeof line, "threads: %d (prontas %d)", ultra_stats.threads, ultra_stats.threads_ready);
    SDL_RenderDebugText(app->renderer, 8, 80, line);
    SDL_snprintf(line, sizeof line, "framebuffer: %08x  audio: %u Hz", ultra_stats.framebuffer,
                 ultra_stats.ai_frequency);
    SDL_RenderDebugText(app->renderer, 8, 96, line);
    SDL_RenderPresent(app->renderer);
    return 1;
}

int main(int argc, char **argv) {
    // ROM path: first argument, or "rom.z64" next to the executable.
    char default_path[1024];
    const char *base = SDL_GetBasePath();
    SDL_snprintf(default_path, sizeof(default_path), "%srom.z64", base ? base : "");
    const char *rom_path = argc > 1 ? argv[1] : default_path;

    static App app;
    RomStatus status = rom_load(rom_path, &app.rom);
    if (status != ROM_OK) {
        char msg[1024];
        SDL_snprintf(msg, sizeof(msg), "%s\n\nArquivo: %s%s%s", rom_status_message(status), rom_path,
                     app.rom.data ? "\nSHA-1: " : "", app.rom.data ? app.rom.sha1 : "");
        fail(msg);
        rom_free(&app.rom);
        return 1;
    }
    printf("%s\n  titulo: %s  codigo: %s  sha1: %s\n", rom_status_message(status), app.rom.title,
           app.rom.game_code, app.rom.sha1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fail(SDL_GetError());
        return 1;
    }
    char title[128];
    SDL_snprintf(title, sizeof(title), "MK64 Port - %s (%s)", app.rom.title, app.rom.game_code);
    if (!SDL_CreateWindowAndRenderer(title, 640, 480, SDL_WINDOW_RESIZABLE, &app.window, &app.renderer)) {
        fail(SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(app.renderer, 320, 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    input_init();

    uint8_t *rdram = calloc(RDRAM_SIZE, 1);
    UltraHost host = {on_retrace, NULL, &app};
    ultra_run(rdram, app.rom.data, app.rom.size, &host);  // never returns
    return 0;
}
