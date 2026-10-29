#ifndef CONFIG_H
#define CONFIG_H

// Keyboard bindings, in the order of the tecla_* entries of mk64.ini.
enum {
    CFG_A, CFG_B, CFG_Z, CFG_START, CFG_L, CFG_R,
    CFG_CU, CFG_CD, CFG_CL, CFG_CR,
    CFG_DU, CFG_DD, CFG_DL, CFG_DR,
    CFG_STICK_UP, CFG_STICK_DOWN, CFG_STICK_LEFT, CFG_STICK_RIGHT,
    CFG_KEY_COUNT
};

typedef struct Config {
    int scale;       // window size as a multiple of 320x240
    int fullscreen;
    int smooth;      // linear filtering instead of nearest
    int volume;      // 0..100
    int internal;    // internal resolution, multiple of 320x240 (1-4)
    int widescreen;  // 16:9 picture (wider view of the 3D scene)
    int keys[CFG_KEY_COUNT];  // SDL_Scancode
} Config;

extern Config config;
void config_load(void);

#endif
