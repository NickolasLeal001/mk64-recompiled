// Entry point: verify the user's ROM, open the window and run the recompiled game.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#include "abi.h"
#include "config.h"
#include "gpu.h"
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

// FPS counter: 3x5 glyphs, one column apart, inside a 1-pixel black frame.
#define HUD_H 7
#define HUD_MAX_W 64

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    SDL_Texture *screen_hi;  // internal resolution above 1x
    int gpu;                 // SDL_gpu (Vulkan) presents the picture instead of the SDL_Renderer
    SDL_GPUTexture *gpu_shown;  // what gpu_present shows
    uint32_t *hpixels;
    int showing_hi, hi_w, hi_h;
    int log_w;  // logical width: 320 at 4:3, wider for wider aspect ratios (240 lines high)
    int interp;                 // 60 fps: an in-between image before each new 30 fps frame
    uint32_t last_fb;           // framebuffer of the previous retrace
    int held, prev_held;        // retraces the current / previous framebuffer stayed on screen
    uint64_t interp_shown;      // last in-between image shown
    uint8_t *rdram;
    uint32_t pixels[FB_W * FB_H];
    int show_status;
    uint64_t shown_id;     // image on screen (framebuffer, or in-between stamp), for the fps count
    int fps_images;        // new images shown since fps_t0
    uint64_t fps_t0;
    int fps;               // last count
    uint32_t hud[HUD_MAX_W * HUD_H];  // the fps text (ABGR)
    int hud_w;
    int hud_dirty;
    SDL_Texture *hud_tex;
    SDL_GPUTexture *hud_gpu;
    int shots;  // MK64_SHOTS=N[:from[:to]]: save a BMP every N retraces within [from, to]
    uint64_t shots_from, shots_to;
    uint64_t exit_at;  // MK64_EXIT_AT: stop after this retrace
    SDL_AudioStream *audio;
    uint32_t audio_freq, audio_freq_out;  // rate of the game, rate the stream was set up for
    FILE *wav;  // MK64_WAV: copy of everything sent to the speakers
    uint32_t wav_bytes;
    uint32_t underruns;  // buffers that found the speaker queue already empty
    uint64_t audio_bytes;  // sent to the speakers
    int audio_started;   // playback begins once AUDIO_CUSHION_MS are queued
    double ai_end[2];    // when each buffer in the emulated AI DMA queue finishes (seconds)
    int ai_count;
    Rom rom;
    char title[128];
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

// Audio output. The game sizes each buffer from osAiGetLength, which on the console tells what is
// left of the buffer being played, so it is answered from an emulated AI DMA queue clocked by
// real time; the game then produces audio at exactly the playback rate. The speakers get the
// buffers through an SDL stream that keeps a cushion of AUDIO_CUSHION_MS against scheduling
// jitter, held by nudging the playback speed (at most 0.5%) instead of dropping or padding.
#define AUDIO_CUSHION_MS 60.0

static double now_s(void) { return (double)SDL_GetTicksNS() / 1e9; }

static void ai_advance(App *app, double now) {
    while (app->ai_count && app->ai_end[0] <= now) {
        app->ai_end[0] = app->ai_end[1];
        app->ai_count--;
    }
}

static void on_ai_buffer(void *user, uint32_t addr, uint32_t size, uint32_t freq) {
    App *app = user;
    if (!freq || size == 0 || size > 0x10000) return;
    app->audio_freq = freq;
    if (!ultra_deterministic && (!app->audio || app->audio_freq_out != freq)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)freq};
        if (!app->audio) {
            app->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (app->audio) {
                SDL_SetAudioStreamGain(app->audio, config.volume / 100.0f);
            }
        } else {
            SDL_SetAudioStreamFormat(app->audio, &spec, NULL);
        }
        app->audio_freq_out = freq;
    }
    static int16_t pcm[0x8000];
    uint32_t n = size / 2;
    for (uint32_t k = 0; k < n; k++) pcm[k] = (int16_t)mem_r16(app->rdram, addr + 2 * k);
    if (app->audio) {
        double now = now_s();
        ai_advance(app, now);
        double start = app->ai_count ? app->ai_end[app->ai_count - 1] : now;
        if (app->ai_count == 2) app->ai_end[0] = app->ai_end[1], app->ai_count = 1;  // console queue holds two
        app->ai_end[app->ai_count++] = (start > now ? start : now) + size / 4.0 / freq;
        int queued = SDL_GetAudioStreamQueued(app->audio);
        if (app->audio_started && queued == 0) app->underruns++;
        SDL_PutAudioStreamData(app->audio, pcm, (int)size);
        app->audio_bytes += size;
        double ms = (queued + (int)size) / 4.0 * 1000.0 / freq;
        if (!app->audio_started && ms >= AUDIO_CUSHION_MS) {
            SDL_ResumeAudioStreamDevice(app->audio);
            app->audio_started = 1;
        }
        double nudge = (ms - AUDIO_CUSHION_MS) / AUDIO_CUSHION_MS * 0.005;
        SDL_SetAudioStreamFrequencyRatio(app->audio, 1.0f + (float)SDL_clamp(nudge, -0.005, 0.005));
    }
    if (app->wav) {
        fwrite(pcm, 1, size, app->wav);
        app->wav_bytes += size;
    }
}

// osAiGetLength: bytes left in the buffer the emulated AI is playing (0 without sound output).
// While the speaker cushion is short (a slow frame held the game back), it reports less, so the
// game makes larger buffers until the cushion is back.
static uint32_t ai_queued(void *user) {
    App *app = user;
    if (!app->audio || !app->audio_freq) return 0;
    double now = now_s();
    ai_advance(app, now);
    if (!app->ai_count) return 0;
    double left = (app->ai_end[0] - now) * app->audio_freq * 4.0;
    double cushion = AUDIO_CUSHION_MS / 1000.0 * app->audio_freq * 4.0;
    double queued = SDL_GetAudioStreamQueued(app->audio);
    if (app->audio_started && queued < cushion) left -= (cushion - queued) / 2;
    return left > 0 ? (uint32_t)left & ~7u : 0;
}

// Graphics tasks run as the game starts them, so the game's threads (sound included) wait while a
// frame is drawn. The in-between image is skipped for a frame when the last real frame plus the
// last in-between one took more than INTERP_BUDGET_MS, which keeps heavy scenes (the intro at 2x)
// within the 33 ms of a 30 fps frame instead of starving the sound.
#define INTERP_BUDGET_MS 25.0

static double ms_since(uint64_t t0) {
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void render_task(App *app, uint32_t dl) {
    // MK64_TRACE=N: write every command of the first graphics task after retrace N to dl_trace.txt
    static int traced;
    const char *trace = SDL_getenv("MK64_TRACE");
    if (trace && !traced && ultra_stats.retraces >= (uint64_t)SDL_atoi(trace)) {
        rsp_trace = fopen("dl_trace.txt", "w");
        traced = 1;
    }
    static double real_ms, interp_ms, total_ms;
    static int frames;
    uint64_t t0 = SDL_GetPerformanceCounter();
    rsp_run_display_list(app->rdram, dl);
    real_ms = ms_since(t0);
    // Deterministic runs always interpolate (the time measured varies from run to run).
    if (app->interp && (ultra_deterministic || real_ms + interp_ms <= INTERP_BUDGET_MS)) {
        uint64_t t1 = SDL_GetPerformanceCounter();
        rsp_run_display_list_interp(app->rdram, dl, 0.5f);
        interp_ms = ms_since(t1);
    } else {
        interp_ms *= 0.9;  // try again once things look lighter
    }
    // MK64_GPU_TIMING: wait for the GPU, so the time below covers its work too (measurements only).
    static int gpu_timing = -1;
    if (gpu_timing < 0) gpu_timing = SDL_getenv("MK64_GPU_TIMING") != NULL;
    if (app->gpu && gpu_timing) gpu_wait_idle();
    total_ms += ms_since(t0);
    if (++frames == 300) {
        ultra_log("render: %.2f ms por quadro (media de 300)", total_ms / frames);
        total_ms = 0;
        frames = 0;
    }
    if (rsp_trace) {
        fclose(rsp_trace);
        rsp_trace = NULL;
    }
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
    if (type == 1) render_task(app, mem_r32(app->rdram, task + 48));
}

static void save_bmp(const char *name, int w, int h, uint32_t *pixels) {
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4);
    if (s) {
        SDL_SaveBMP(s, name);
        SDL_DestroySurface(s);
    }
}

// The 1x frame (the one hashed for replays) and, with a higher internal resolution, the enlarged one.
static void save_shot(App *app) {
    char name[64];
    SDL_snprintf(name, sizeof name, "frame_%05llu.bmp", (unsigned long long)ultra_stats.retraces);
    save_bmp(name, FB_W, FB_H, app->pixels);
    if (app->showing_hi) {
        SDL_snprintf(name, sizeof name, "frame_%05llu_hi.bmp", (unsigned long long)ultra_stats.retraces);
        save_bmp(name, app->hi_w, app->hi_h, app->hpixels);
    }
}

// The counter text into app->hud: digits and "FPS" in yellow on black.
static void hud_build(App *app, const char *text) {
    static const char chars[] = "0123456789FPS";
    static const uint16_t glyphs[] = {075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757,
                                      075717, 074644, 075744, 034216};
    int n = (int)SDL_strlen(text);
    app->hud_w = SDL_min(1 + 4 * n, HUD_MAX_W);
    for (int k = 0; k < app->hud_w * HUD_H; k++) app->hud[k] = 0xFF000000u;
    for (int c = 0; c < n && 4 * c + 4 <= app->hud_w; c++) {
        const char *at = SDL_strchr(chars, text[c]);
        if (!at || !text[c]) continue;  // space
        uint16_t g = glyphs[at - chars];
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 3; x++)
                if (g >> (14 - 3 * y - x) & 1) app->hud[(1 + y) * app->hud_w + 1 + 4 * c + x] = 0xFF00FFFFu;
    }
    app->hud_dirty = 1;
}

// Counts the new images shown (the in-between ones included) and refreshes the text twice a second.
static void fps_update(App *app, uint64_t id) {
    if (id != app->shown_id) {
        app->shown_id = id;
        app->fps_images++;
    }
    uint64_t now = SDL_GetTicksNS();
    if (!app->fps_t0) app->fps_t0 = now;
    if (now - app->fps_t0 < 500000000ull) return;
    int fps = (int)(app->fps_images * 1e9 / (double)(now - app->fps_t0) + 0.5);
    app->fps_images = 0;
    app->fps_t0 = now;
    if (fps == app->fps) return;
    app->fps = fps;
    char text[16];
    SDL_snprintf(text, sizeof text, "%d FPS", fps);
    hud_build(app, text);
}

// Each retrace shows the framebuffer the game handed to osViSwapBuffer (F1: status overlay).
// With interpolation, a frame that replaces one held for two or more retraces (the game draws at
// 30 fps or less) is first shown as its in-between image, and itself on the next retrace.
static int on_retrace(void *user) {
    App *app = user;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) return 0;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1) {
            app->show_status = !app->show_status;
            if (!app->show_status && app->gpu && app->window) SDL_SetWindowTitle(app->window, app->title);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F2) config.show_fps = !config.show_fps;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && app->window) {
            config.fullscreen = !config.fullscreen;
            SDL_SetWindowFullscreen(app->window, config.fullscreen);
        }
    }
    input_update();
    if (app->audio && ultra_stats.retraces % 600 == 0) {
        static uint64_t last_bytes;
        ultra_log("audio: fila %.0f ms, %u vezes vazia, %.1f s de som nos ultimos 600 retraces",
                  SDL_GetAudioStreamQueued(app->audio) / 4.0 * 1000.0 / app->audio_freq, app->underruns,
                  (app->audio_bytes - last_bytes) / 4.0 / app->audio_freq);
        last_bytes = app->audio_bytes;
    }
    static uint32_t reported_ops;
    if (abi_unknown_ops != reported_ops) {
        reported_ops = abi_unknown_ops;
        ultra_log("audio: opcodes sem tratamento (bits) 0x%08x", abi_unknown_ops);
    }
    uint64_t n = ultra_stats.retraces;
    if (ultra_stats.framebuffer) {
        uint32_t fb = ultra_stats.framebuffer;
        int swapped = fb != app->last_fb;
        if (swapped) {
            app->prev_held = app->held;
            app->held = 0;
            app->last_fb = fb;
        }
        app->held++;
        rdp_read_framebuffer(fb, FB_W, FB_H, app->pixels);
        if (app->screen) SDL_UpdateTexture(app->screen, NULL, app->pixels, FB_W * 4);
        int hw = 0, hh = 0;
        int want_mid = app->interp && swapped && app->prev_held >= 2;
        int shot = app->shots && n % (uint64_t)app->shots == 0 && n >= app->shots_from && n <= app->shots_to;
        uint64_t shown = fb;  // the frame itself, or its in-between image (high bit set)
        if (app->gpu) {
            // The GPU path hands over textures; screenshots read them back.
            uint64_t mid = 0;
            SDL_GPUTexture *tex = want_mid ? rdp_gpu_frame(fb, FB_W, FB_H, 1, &mid, &hw, &hh) : NULL;
            if (tex && mid != app->interp_shown) app->interp_shown = mid, shown = mid | 1ull << 63;
            else tex = rdp_gpu_frame(fb, FB_W, FB_H, 0, NULL, &hw, &hh);
            app->showing_hi = tex != NULL;
            app->gpu_shown = tex ? tex : app->window ? gpu_upload_image(0, app->pixels, FB_W, FB_H) : NULL;
            if (tex && shot && app->hpixels) gpu_download(tex, hw, hh, app->hpixels);
        } else {
            uint64_t mid = want_mid ? rdp_read_framebuffer_interp(fb, FB_W, FB_H, app->hpixels, &hw, &hh) : 0;
            if (mid && mid != app->interp_shown) {
                app->interp_shown = mid;
                app->showing_hi = 1;
                shown = mid | 1ull << 63;
            } else {
                app->showing_hi = app->hpixels && rdp_read_framebuffer_hires(fb, FB_W, FB_H, app->hpixels, &hw, &hh);
            }
        }
        if (app->showing_hi && app->renderer) {
            if (!app->screen_hi || hw != app->hi_w || hh != app->hi_h) {
                if (app->screen_hi) SDL_DestroyTexture(app->screen_hi);
                app->screen_hi = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, hw, hh);
                SDL_SetTextureScaleMode(app->screen_hi, config.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            }
            SDL_UpdateTexture(app->screen_hi, NULL, app->hpixels, hw * 4);
        }
        app->hi_w = hw;
        app->hi_h = hh;
        fps_update(app, shown);
        if (shot) {
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
        int near_end = progress >= 2.97f && progress < 4.0f && !(flags & 0x0800);  // until it crosses the line
        // The lap counter (0x80164390) is -1 until the race is under way. Taking over earlier
        // can leave no human player when the start is set up, and then no Lakitu comes to count
        // down: player 1 only accelerates until then.
        int32_t laps = (int32_t)mem_r32(app->rdram, 0x80164390u);
        if (laps < 0 && !(flags & 0x0800)) {
            ultra_pads[0].buttons |= 0x8000;
        } else if (driving && near_end) {
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
    // MK64_POKE=n[-m]:addr=value[;...]: store a 32-bit value at retrace n (or every retrace from n
    // to m), for tests that need a game state the scripted input cannot reach.
    for (const char *p = SDL_getenv("MK64_POKE"); p && *p; p = SDL_strchr(p, ';') ? SDL_strchr(p, ';') + 1 : "") {
        const char *colon = SDL_strchr(p, ':'), *eq = colon ? SDL_strchr(colon, '=') : NULL;
        const char *dash = SDL_strchr(p, '-');
        uint64_t from = (uint64_t)SDL_atoi(p), to = dash && dash < colon ? (uint64_t)SDL_atoi(dash + 1) : from;
        if (eq && n >= from && n <= to)
            mem_w32(app->rdram, (uint32_t)SDL_strtoul(colon + 1, NULL, 16), (uint32_t)SDL_strtoul(eq + 1, NULL, 16));
    }
    // MK64_DUMPRAM=n[,n...]: write RDRAM in N64 byte order to ram_<n>.bin (debugging and analysis).
    const char *dump = SDL_getenv("MK64_DUMPRAM");
    int dump_now = 0;
    for (const char *d = dump; d && *d; d = SDL_strchr(d, ',') ? SDL_strchr(d, ',') + 1 : "")
        dump_now |= n == (uint64_t)SDL_atoi(d);
    if (dump_now) {
        char name[64];
        SDL_snprintf(name, sizeof name, "ram_%llu.bin", (unsigned long long)n);
        FILE *f = fopen(name, "wb");
        if (f) {
            for (uint32_t a = 0; a < RDRAM_SIZE; a++) fputc(mem_r8(app->rdram, 0x80000000u + a), f);
            fclose(f);
        }
    }
    if (app->exit_at && n >= app->exit_at) return 0;
    if (app->gpu && app->window) {
        SDL_GPUTexture *hud = NULL;
        if (config.show_fps && app->hud_w) {
            if (app->hud_dirty || !app->hud_gpu) app->hud_gpu = gpu_upload_image(1, app->hud, app->hud_w, HUD_H);
            app->hud_dirty = 0;
            hud = app->hud_gpu;
        }
        if (app->showing_hi) gpu_present(app->gpu_shown, app->hi_w, app->hi_h, app->log_w, config.smooth, hud, app->hud_w, HUD_H);
        else gpu_present(app->gpu_shown, FB_W, FB_H, app->log_w, config.smooth, hud, app->hud_w, HUD_H);
        if (app->show_status) {  // no text drawing on this path: the status goes to the title bar
            char line[160];
            SDL_snprintf(line, sizeof line, "MK64 Port - retraces %llu, triangulos %llu, framebuffer %08x",
                         (unsigned long long)ultra_stats.retraces, (unsigned long long)rsp_stats_tris, ultra_stats.framebuffer);
            SDL_SetWindowTitle(app->window, line);
        }
        return 1;
    }
    if (!app->renderer) return 1;  // headless
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    // A widened display copy fills the logical area; a 4:3 one, or the 1x frame, sits in its middle.
    SDL_FRect mid = {(app->log_w - FB_W) / 2.0f, 0, FB_W, FB_H};
    int wide = app->showing_hi && app->hi_w * 3 > app->hi_h * 4;
    if (wide) SDL_RenderTexture(app->renderer, app->screen_hi, NULL, NULL);
    else SDL_RenderTexture(app->renderer, app->showing_hi ? app->screen_hi : app->screen, NULL, &mid);
    if (config.show_fps && app->hud_w) {
        if (app->hud_dirty || !app->hud_tex) {
            if (app->hud_tex && app->hud_tex->w != app->hud_w) SDL_DestroyTexture(app->hud_tex), app->hud_tex = NULL;
            if (!app->hud_tex) {
                app->hud_tex = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, app->hud_w, HUD_H);
                SDL_SetTextureScaleMode(app->hud_tex, SDL_SCALEMODE_NEAREST);
            }
            SDL_UpdateTexture(app->hud_tex, NULL, app->hud, app->hud_w * 4);
            app->hud_dirty = 0;
        }
        SDL_FRect at = {(wide ? 0 : mid.x) + HUD_MARGIN, HUD_MARGIN, app->hud_w * 2.0f, HUD_H * 2.0f};
        SDL_RenderTexture(app->renderer, app->hud_tex, NULL, &at);
    }
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
    config_load();
    // Aspect ratio: mk64.ini, or MK64_ASPECT ("21:9"; MK64_WIDESCREEN=1 is 16:9). Headless runs
    // default to 4:3.
    float aspect = !headless ? config.aspect : 4.0f / 3.0f;
    const char *wide_env = SDL_getenv("MK64_WIDESCREEN"), *aspect_env = SDL_getenv("MK64_ASPECT");
    if (wide_env) aspect = SDL_atoi(wide_env) ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (aspect_env) aspect = config_parse_aspect(aspect_env, aspect);
    app.log_w = (int)(FB_H * aspect + 0.5f);
    if (app.log_w < FB_W + 2) app.log_w = FB_W;
    // Renderer of the display picture: mk64.ini, or MK64_RENDERER (gpu / cpu). Headless runs
    // default to cpu. Without a usable Vulkan device the software renderer takes over.
    const char *renderer_env = SDL_getenv("MK64_RENDERER");
    int want_gpu = renderer_env ? SDL_strcmp(renderer_env, "gpu") == 0 : !headless && config.gpu;
    if (!headless) {
        SDL_snprintf(app.title, sizeof(app.title), "MK64 Port - %s (%s)", app.rom.title, app.rom.game_code);
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (config.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        app.window = SDL_CreateWindow(app.title, app.log_w * config.scale, FB_H * config.scale, flags);
        if (!app.window) {
            fail(SDL_GetError());
            return 1;
        }
    }
    if (want_gpu) {
        app.gpu = gpu_init(app.window, SDL_getenv("MK64_GPU_DEBUG") != NULL);
        if (!app.gpu) ultra_log("GPU indisponivel (%s): usando o renderizador em software", gpu_error());
    }
    if (!headless && !app.gpu) {
        app.renderer = SDL_CreateRenderer(app.window, NULL);
        if (!app.renderer) {
            fail(SDL_GetError());
            return 1;
        }
        SDL_SetRenderVSync(app.renderer, 0);
        SDL_SetRenderLogicalPresentation(app.renderer, app.log_w, FB_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        app.screen = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
        SDL_SetTextureScaleMode(app.screen, config.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
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
    // Internal resolution: mk64.ini, or MK64_RES_SCALE (headless runs default to 1x).
    const char *res = SDL_getenv("MK64_RES_SCALE");
    int max_internal = app.gpu ? 8 : 4;
    int internal = SDL_clamp(res ? SDL_atoi(res) : headless ? 1 : config.internal, 1, max_internal);
    int wide = app.log_w != FB_W;
    int max_w = SDL_max(2 * FB_W, app.log_w + 2);  // widest display copy, in 1x pixels
    // Interpolation: mk64.ini, or MK64_INTERP (headless runs default to off).
    const char *interp = SDL_getenv("MK64_INTERP");
    app.interp = interp ? SDL_atoi(interp) != 0 : !headless && config.interp;
    rdp_set_gpu(app.gpu);
    rdp_set_scale(internal, wide ? aspect : 4.0f / 3.0f, app.interp);
    // Display copies: read into hpixels by the software path; on the GPU only for screenshots.
    if ((internal > 1 || wide || app.interp) && (!app.gpu || app.shots)) app.hpixels = malloc(sizeof(uint32_t) * max_w * FB_H * internal * internal);
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
