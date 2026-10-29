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
    fprintf(f, "# Resolucao interna: 1 = 320x240 original, 2 = 640x480, ate 4 em cpu ou 8 em gpu\n");
    fprintf(f, "resolucao_interna = 2\n\n");
    fprintf(f, "# Quem desenha a imagem da tela: gpu (Vulkan; resolucao interna ate 8) ou cpu (software).\n");
    fprintf(f, "# Sem Vulkan, o jogo volta sozinho para cpu.\n");
    fprintf(f, "renderizador = gpu\n\n");
    fprintf(f, "# Proporcao da imagem: 4:3 (original), 16:9, 16:10, 21:9, 32:9 ou outra no formato a:b.\n");
    fprintf(f, "# Mais larga mostra mais da pista nas laterais; o HUD fica no centro.\n");
    fprintf(f, "proporcao = 4:3\n\n");
    fprintf(f, "# 60 quadros por segundo: imagens intermediarias entre os quadros do jogo (30 por segundo)\n");
    fprintf(f, "interpolacao = 1\n\n");
    fprintf(f, "# Contador de quadros por segundo no canto da imagem (F2 alterna)\nmostrar_fps = 1\n\n");
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

// "a:b" (or a plain number) as width / height, from 4:3 up to 4:1; `fallback` when unreadable.
float config_parse_aspect(const char *text, float fallback) {
    float a = 0, b = 1;
    if (!text || SDL_sscanf(text, "%f:%f", &a, &b) < 1 || a <= 0 || b <= 0) return fallback;
    float r = a / b;
    return r < 4.0f / 3.0f ? 4.0f / 3.0f : r > 4.0f ? 4.0f : r;
}

void config_load(void) {
    config.scale = 3;
    config.fullscreen = 0;
    config.smooth = 0;
    config.volume = 100;
    config.internal = 2;
    config.aspect = 4.0f / 3.0f;
    config.interp = 1;
    config.gpu = 1;
    config.show_fps = 1;
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
        else if (!strcmp(key, "resolucao_interna")) config.internal = SDL_clamp(SDL_atoi(val), 1, 8);
        else if (!strcmp(key, "tela_larga")) config.aspect = SDL_atoi(val) ? 16.0f / 9.0f : 4.0f / 3.0f;  // older files
        else if (!strcmp(key, "proporcao")) config.aspect = config_parse_aspect(val, config.aspect);
        else if (!strcmp(key, "renderizador")) config.gpu = strcmp(val, "cpu") != 0;
        else if (!strcmp(key, "interpolacao")) config.interp = SDL_atoi(val) != 0;
        else if (!strcmp(key, "mostrar_fps")) config.show_fps = SDL_atoi(val) != 0;
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
