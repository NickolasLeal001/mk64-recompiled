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
    int internal;    // internal resolution, multiple of 320x240 (1-4; up to 8 on the GPU)
    float aspect;    // picture width / height: 4:3, or wider for a wider view of the 3D scene
    int interp;      // 60 fps display through in-between images
    int gpu;         // display copies drawn by the GPU (Vulkan) instead of the software renderer
    int keys[CFG_KEY_COUNT];  // SDL_Scancode
} Config;

extern Config config;
void config_load(void);
float config_parse_aspect(const char *text, float fallback);

#endif
