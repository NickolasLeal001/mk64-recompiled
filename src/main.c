// Entry point: verify the user's ROM, open the window and run the recompiled game.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#include "input.h"
#include "rdp.h"
#include "rom.h"
#include "rsp.h"
#include "ultra.h"

#define FB_W 320
#define FB_H 240

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MK64 Port", msg, NULL);
}

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    uint8_t *rdram;
    uint32_t pixels[FB_W * FB_H];
    int show_status;
    int shots;  // MK64_SHOTS=N[:from[:to]]: save a BMP every N retraces within [from, to]
    uint64_t shots_from, shots_to;
    Rom rom;
} App;

static void on_task(void *user, uint32_t task) {
    App *app = user;
    if (mem_r32(app->rdram, task) != 1) return;
    // MK64_TRACE=N: write every command of the first graphics task after retrace N to dl_trace.txt
    static int traced;
    const char *trace = SDL_getenv("MK64_TRACE");
    if (trace && !traced && ultra_stats.retraces >= (uint64_t)SDL_atoi(trace)) {
        rsp_trace = fopen("dl_trace.txt", "w");
        traced = 1;
    }
    rsp_run_display_list(app->rdram, mem_r32(app->rdram, task + 48));
    if (rsp_trace) {
        fclose(rsp_trace);
        rsp_trace = NULL;
    }
}

static void save_shot(App *app) {
    char name[64];
    SDL_snprintf(name, sizeof name, "frame_%05llu.bmp", (unsigned long long)ultra_stats.retraces);
    SDL_Surface *s = SDL_CreateSurfaceFrom(FB_W, FB_H, SDL_PIXELFORMAT_ABGR8888, app->pixels, FB_W * 4);
    if (s) {
        SDL_SaveBMP(s, name);
        SDL_DestroySurface(s);
    }
}

// Each retrace shows the framebuffer the game handed to osViSwapBuffer (F1: status overlay).
static int on_retrace(void *user) {
    App *app = user;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) return 0;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1) app->show_status = !app->show_status;
    }
    input_update();
    if (ultra_stats.framebuffer) {
        rdp_read_framebuffer(ultra_stats.framebuffer, FB_W, FB_H, app->pixels);
        SDL_UpdateTexture(app->screen, NULL, app->pixels, FB_W * 4);
        uint64_t n = ultra_stats.retraces;
        if (app->shots && n % (uint64_t)app->shots == 0 && n >= app->shots_from && n <= app->shots_to) save_shot(app);
    }
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    SDL_RenderTexture(app->renderer, app->screen, NULL, NULL);
    if (!app->show_status) {
        SDL_RenderPresent(app->renderer);
        return 1;
    }
    char line[128];
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 0, 255);
    SDL_snprintf(line, sizeof line, "triangulos: %llu", (unsigned long long)rsp_stats_tris);
    SDL_RenderDebugText(app->renderer, 8, 112, line);
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
    SDL_SetRenderLogicalPresentation(app.renderer, FB_W, FB_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    app.screen = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    SDL_SetTextureScaleMode(app.screen, SDL_SCALEMODE_NEAREST);
    const char *shots = SDL_getenv("MK64_SHOTS");
    app.shots = shots ? SDL_atoi(shots) : 0;
    app.shots_to = (uint64_t)-1;
    if (shots && SDL_strchr(shots, ':')) {
        const char *from = SDL_strchr(shots, ':') + 1;
        app.shots_from = (uint64_t)SDL_atoi(from);
        if (SDL_strchr(from, ':')) app.shots_to = (uint64_t)SDL_atoi(SDL_strchr(from, ':') + 1);
    }
    input_init();

    app.rdram = calloc(RDRAM_SIZE, 1);
    rdp_reset(app.rdram);
    rsp_reset();
    UltraHost host = {on_retrace, on_task, &app};
    ultra_run(app.rdram, app.rom.data, app.rom.size, &host);  // never returns
    return 0;
}
