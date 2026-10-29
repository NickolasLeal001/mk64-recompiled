// mk64.ini next to the executable: window, audio and keyboard mapping. Written with the defaults
// the first time, so it can be edited by hand. Format: "key = value" lines, '#' comments.
#include "config.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

Config config;

static const struct { const char *key; const char *def; } KEYS[CFG_KEY_COUNT] = {
    {"A", "X"}, {"B", "C"}, {"Z", "Z"}, {"START", "Return"}, {"L", "A"}, {"R", "S"},
    {"C_CIMA", "I"}, {"C_BAIXO", "K"}, {"C_ESQUERDA", "J"}, {"C_DIREITA", "L"},
    {"D_CIMA", "T"}, {"D_BAIXO", "G"}, {"D_ESQUERDA", "F"}, {"D_DIREITA", "H"},
    {"ANALOGICO_CIMA", "Up"}, {"ANALOGICO_BAIXO", "Down"}, {"ANALOGICO_ESQUERDA", "Left"},
    {"ANALOGICO_DIREITA", "Right"},
};

static void path_of(char *out, size_t n) {
    const char *base = SDL_GetBasePath();
    SDL_snprintf(out, n, "%smk64.ini", base ? base : "");
}

static void write_default(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# MK64 Port - configuracao (apague este arquivo para voltar ao padrao)\n\n");
    fprintf(f, "# Janela: escala inteira da imagem 320x240, tela cheia (F11 alterna) e filtro\n");
    fprintf(f, "escala = 3\ntela_cheia = 0\n# filtro: nitido ou suave\nfiltro = nitido\n\n");
    fprintf(f, "# Volume de 0 a 100\nvolume = 100\n\n");
    fprintf(f, "# Teclado: nomes de tecla do SDL (ex.: X, Return, Left Shift, Space, Up)\n");
    for (int k = 0; k < CFG_KEY_COUNT; k++) fprintf(f, "tecla_%s = %s\n", KEYS[k].key, KEYS[k].def);
    fclose(f);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

void config_load(void) {
    config.scale = 3;
    config.fullscreen = 0;
    config.smooth = 0;
    config.volume = 100;
    for (int k = 0; k < CFG_KEY_COUNT; k++) config.keys[k] = SDL_GetScancodeFromName(KEYS[k].def);

    char path[1024];
    path_of(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        write_default(path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '#' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(key, "escala")) config.scale = SDL_clamp(SDL_atoi(val), 1, 8);
        else if (!strcmp(key, "tela_cheia")) config.fullscreen = SDL_atoi(val) != 0;
        else if (!strcmp(key, "filtro")) config.smooth = !strcmp(val, "suave");
        else if (!strcmp(key, "volume")) config.volume = SDL_clamp(SDL_atoi(val), 0, 100);
        else if (!strncmp(key, "tecla_", 6)) {
            for (int k = 0; k < CFG_KEY_COUNT; k++) {
                if (strcmp(key + 6, KEYS[k].key)) continue;
                SDL_Scancode sc = SDL_GetScancodeFromName(val);
                if (sc != SDL_SCANCODE_UNKNOWN) config.keys[k] = sc;
                else SDL_Log("mk64.ini: tecla desconhecida '%s' para %s", val, key);
            }
        }
    }
    fclose(f);
}
