// Entry point: verify the user's ROM, open the window and run the recompiled game.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#include "abi.h"
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
    uint64_t exit_at;  // MK64_EXIT_AT: stop after this retrace
    SDL_AudioStream *audio;
    uint32_t audio_freq;
    FILE *wav;  // MK64_WAV: copy of everything sent to the speakers
    uint32_t wav_bytes;
    Rom rom;
} App;

static App *g_app;

static void wav_header(FILE *f, uint32_t freq, uint32_t data_bytes) {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t riff = 36 + data_bytes, rate = freq * 4;
    SDL_memcpy(h + 4, &riff, 4);
    SDL_memcpy(h + 24, &freq, 4);
    SDL_memcpy(h + 28, &rate, 4);
    SDL_memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void finish_wav(void) {
    if (g_app && g_app->wav) {
        wav_header(g_app->wav, g_app->audio_freq, g_app->wav_bytes);
        fclose(g_app->wav);
        g_app->wav = NULL;
    }
}

static void on_ai_buffer(void *user, uint32_t addr, uint32_t size, uint32_t freq) {
    App *app = user;
    if (!freq || size == 0 || size > 0x10000 || ultra_deterministic) return;
    if (!app->audio || app->audio_freq != freq) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)freq};
        if (!app->audio) {
            app->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (app->audio) SDL_ResumeAudioStreamDevice(app->audio);
        } else {
            SDL_SetAudioStreamFormat(app->audio, &spec, NULL);
        }
        app->audio_freq = freq;
    }
    static int16_t pcm[0x8000];
    uint32_t n = size / 2;
    for (uint32_t k = 0; k < n; k++) pcm[k] = (int16_t)mem_r16(app->rdram, addr + 2 * k);
    if (app->audio) SDL_PutAudioStreamData(app->audio, pcm, (int)size);
    if (app->wav) {
        fwrite(pcm, 1, size, app->wav);
        app->wav_bytes += size;
    }
}

// The game produces a fixed amount of audio per frame; measured queue depth stays near one
// buffer (13-16 ms) during races, so the queued byte count is reported as is.
static uint32_t ai_queued(void *user) {
    App *app = user;
    int q = app->audio ? SDL_GetAudioStreamQueued(app->audio) : 0;
    return q > 0 ? (uint32_t)q : 0;
}

static void on_task(void *user, uint32_t task) {
    App *app = user;
    uint32_t type = mem_r32(app->rdram, task);
    if (type == 2) {
        uint32_t list = mem_r32(app->rdram, task + 48), size = mem_r32(app->rdram, task + 52);
        static int dumped;
        if (SDL_getenv("MK64_DUMP_AL") && ultra_stats.retraces > 900 && dumped < 1) {
            dumped++;
            for (uint32_t k = 0; k < size; k += 8)
                ultra_log("  AL %08x %08x", mem_r32(app->rdram, list + k), mem_r32(app->rdram, list + k + 4));
        }
        abi_run(app->rdram, list, size);
        return;
    }
    if (type != 1) return;
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
    if (app->audio && ultra_stats.retraces % 600 == 0)
        ultra_log("audio: fila %d bytes (%.0f ms)", SDL_GetAudioStreamQueued(app->audio),
                  app->audio_freq ? SDL_GetAudioStreamQueued(app->audio) / 4.0 * 1000.0 / app->audio_freq : 0.0);
    static uint32_t reported_ops;
    if (abi_unknown_ops != reported_ops) {
        reported_ops = abi_unknown_ops;
        ultra_log("audio: opcodes sem tratamento (bits) 0x%08x", abi_unknown_ops);
    }
    uint64_t n = ultra_stats.retraces;
    if (ultra_stats.framebuffer) {
        rdp_read_framebuffer(ultra_stats.framebuffer, FB_W, FB_H, app->pixels);
        if (app->screen) SDL_UpdateTexture(app->screen, NULL, app->pixels, FB_W * 4);
        if (app->shots && n % (uint64_t)app->shots == 0 && n >= app->shots_from && n <= app->shots_to) {
            save_shot(app);
            // FNV-1a of the frame, for replay regression checks.
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < FB_W * FB_H; k++) h = (h ^ app->pixels[k]) * 1099511628211ull;
            FILE *f = fopen("replay_hashes.txt", "a");
            if (f) { fprintf(f, "%llu %016llx\n", (unsigned long long)n, (unsigned long long)h); fclose(f); }
        }
    }
    // MK64_AUTOPILOT=n: from retrace n on, player 1 (0x800F6990) carries the computer-driver flag
    // (0x1000) instead of the human one (0x4000), so the game's own AI drives it. For automated
    // tests that need to finish races.
    // Once the race is over (0x0800 on every kart) player 1 goes back to human, or the result
    // screens would wait forever for a human player.
    // Near the end of the last lap (race progress 0x801644d0[0] >= 2.97 laps) control goes back to
    // the human player, so the game sees a human cross the line and its result screens advance.
    const char *autopilot = SDL_getenv("MK64_AUTOPILOT");
    if (autopilot && n >= (uint64_t)SDL_atoi(autopilot)) {
        static int driving;
        uint16_t flags = mem_r16(app->rdram, 0x800F6990u);
        uint32_t pbits = mem_r32(app->rdram, 0x801644D0u);
        float progress;
        SDL_memcpy(&progress, &pbits, 4);
        int near_end = progress >= 2.97f && progress < 4.0f;
        if (driving && near_end) {
            mem_w16(app->rdram, 0x800F6990u, (uint16_t)((flags & ~0x1000) | 0x4000));
            driving = 0;
            ultra_pads[0].buttons |= 0x8000;
        } else if (near_end) {
            ultra_pads[0].buttons |= 0x8000;  // accelerate across the line
        } else if ((flags & 0x8000) && (flags & 0x4000) && !(flags & 0x0800)) {
            mem_w16(app->rdram, 0x800F6990u, (uint16_t)((flags & ~0x4000) | 0x1000));
            driving = 1;
        } else if (driving && (flags & 0x0800)) {
            mem_w16(app->rdram, 0x800F6990u, (uint16_t)((flags & ~0x1000) | 0x4000));
            driving = 0;
        }
        // Race over: open the post-race menu with START, then take its first option with A.
        if ((flags & 0x0800) && !SDL_getenv("MK64_AUTOPILOT_MANUAL")) {
            uint64_t phase = n % 120;
            if (phase < 4) ultra_pads[0].buttons |= 0x1000;              // START
            else if (phase >= 60 && phase < 64) ultra_pads[0].buttons |= 0x8000;  // A
        }
    }
    // MK64_DUMPRAM=n: write RDRAM in N64 byte order to ram_<n>.bin (debugging and analysis).
    const char *dump = SDL_getenv("MK64_DUMPRAM");
    if (dump && n == (uint64_t)SDL_atoi(dump)) {
        char name[64];
        SDL_snprintf(name, sizeof name, "ram_%llu.bin", (unsigned long long)n);
        FILE *f = fopen(name, "wb");
        if (f) {
            for (uint32_t a = 0; a < RDRAM_SIZE; a++) fputc(mem_r8(app->rdram, 0x80000000u + a), f);
            fclose(f);
        }
    }
    if (app->exit_at && n >= app->exit_at) return 0;
    if (!app->renderer) return 1;  // headless
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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fail(SDL_GetError());
        return 1;
    }
    // MK64_HEADLESS: no window or sound, virtual time (deterministic, faster than real time).
    int headless = SDL_getenv("MK64_HEADLESS") != NULL;
    ultra_deterministic = headless;
    const char *exit_at = SDL_getenv("MK64_EXIT_AT");
    app.exit_at = exit_at ? (uint64_t)SDL_atoi(exit_at) : 0;
    if (!headless) {
        char title[128];
        SDL_snprintf(title, sizeof(title), "MK64 Port - %s (%s)", app.rom.title, app.rom.game_code);
        if (!SDL_CreateWindowAndRenderer(title, 640, 480, SDL_WINDOW_RESIZABLE, &app.window, &app.renderer)) {
            fail(SDL_GetError());
            return 1;
        }
        SDL_SetRenderLogicalPresentation(app.renderer, FB_W, FB_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        app.screen = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
        SDL_SetTextureScaleMode(app.screen, SDL_SCALEMODE_NEAREST);
    }
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
    // The audio microcode's resampling filter, at ROM 0xF5A00 in this (SHA-1 checked) ROM.
    if (!abi_init(app.rom.data, app.rom.size, 0xF5A00)) fprintf(stderr, "aviso: tabela de reamostragem nao encontrada\n");
    g_app = &app;
    const char *wav = SDL_getenv("MK64_WAV");
    if (wav && (app.wav = fopen(wav, "wb")) != NULL) {
        wav_header(app.wav, 0, 0);
        atexit(finish_wav);
    }
    UltraHost host = {on_retrace, on_task, on_ai_buffer, ai_queued, &app};
    ultra_run(app.rdram, app.rom.data, app.rom.size, &host);  // never returns
    return 0;
}
