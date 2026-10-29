// Readable rewrites of the kart physics (phase 9), replacing their mechanical translation.
// Each one must leave the same state as the original: tests/recomp_diff.c compares it with the
// MIPS interpreter (registers of its ABI contract and all of RDRAM, stack frames included).
#include <string.h>

#include "funcs.h"

// --- Player struct (0xDD8 bytes per kart, array at 0x800F6990): fields used here -------------
#define PLAYER_STATUS 0x0C        // u32; 0x10000000: a pending engine-sound update
#define PLAYER_UPDATE_FLAGS 0x44  // s16; 0x20: the target speed was updated this frame
#define PLAYER_SPEED_SQUARED 0x8C // f32: target speed squared / 25 (engine pitch and effects)
#define PLAYER_TARGET_SPEED 0x9C  // f32: the speed the engine pulls towards
#define PLAYER_STATE 0xBC         // u32; 0x8 and 0x40000000 decide whether 0x8C is refreshed
#define PLAYER_SLOPE 0xC4         // s16 angle, 65536 units per turn: the kart's pitch on a slope
#define PLAYER_TOP_SPEED 0x214    // f32
#define PLAYER_CHARACTER 0x254    // u16, 0-7

// Per character: a pointer to 10 floats, what each tenth of the top speed adds to the target.
#define ACCELERATION_TABLES 0x800E3010u
// Per character: the factor applied to every step while the kart's boosted flag is on.
#define BOOST_FACTORS 0x800E36B0u
// Per player index (FUN_80027d00): nonzero while that kart accelerates boosted.
#define BOOSTED_PLAYERS 0x80165460u

#define ACCELERATION_BANDS 10
#define SLOPE_GAIN 0.05  // per degree of pitch
#define UNITS_PER_DEGREE 182  // 65536 / 360, truncated like the game does

static float f32_at(uint8_t *ram, uint32_t addr) {
    uint32_t bits = mem_r32(ram, addr);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void f32_put(uint8_t *ram, uint32_t addr, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    mem_w32(ram, addr, bits);
}

// Calls a game function the way the original does: same stack pointer and return address,
// since the callee saves them into the stack.
static void call_game(uint8_t *ram, Ctx *ctx, RecompFunc fn, uint32_t sp, uint32_t return_addr) {
    ctx->r[29] = SX32(sp);
    ctx->r[31] = SX32(return_addr);
    fn(ram, ctx);
}

// The target speed climbs in ten bands of a tenth of the top speed each. Every band the target is
// in adds the character's value for it plus 0.05 per degree of slope (more uphill, less downhill);
// the bands are checked in order, so a step that crosses into the next band can add again in the
// same frame. While boosted, each step is multiplied by the character's boost factor.
static float accelerate(uint8_t *ram, uint32_t player, float target, int boosted) {
    uint32_t character = mem_r16(ram, player + PLAYER_CHARACTER);
    uint32_t table = mem_r32(ram, ACCELERATION_TABLES + 4 * character);
    double factor = boosted ? f32_at(ram, BOOST_FACTORS + 4 * character) : 1.0;
    for (int band = 0; band < ACCELERATION_BANDS; band++) {
        double top = f32_at(ram, player + PLAYER_TOP_SPEED);
        double now = target;
        double from = band == 0 ? 0.0 : top * (band / 10.0);
        int inside = band == ACCELERATION_BANDS - 1 ? now <= top : now < top * ((band + 1) / 10.0);
        if (!(from <= now && inside)) continue;
        int degrees = (int16_t)mem_r16(ram, player + PLAYER_SLOPE) / UNITS_PER_DEGREE;
        double step = (double)f32_at(ram, table + 4 * (uint32_t)band) + SLOPE_GAIN * degrees;
        if (boosted) step *= factor;
        target = fnan_s((float)(now + step));  // a NaN becomes the R4300's default one
        f32_put(ram, player + PLAYER_TARGET_SPEED, target);
        target = f32_at(ram, player + PLAYER_TARGET_SPEED);
    }
    return target;
}

// void FUN_80030fc8(Player *player): the engine's target speed for this frame.
void func_80030FC8(uint8_t *ram, Ctx *ctx) {
    uint32_t player = (uint32_t)ctx->r[4];
    uint32_t sp = (uint32_t)ctx->r[29], frame = sp - 24, ra = (uint32_t)ctx->r[31];
    mem_w32(ram, frame + 20, ra);
    mem_w32(ram, frame + 24, player);  // the caller's home slot for the argument

    call_game(ram, ctx, func_80027D00, frame, 0x80030FD8u);  // the kart's player index
    int index = (int)(int32_t)ctx->r[2];
    int boosted = mem_r32(ram, BOOSTED_PLAYERS + 4 * (uint32_t)index) != 0;

    float target = accelerate(ram, player, f32_at(ram, player + PLAYER_TARGET_SPEED), boosted);

    // Kept between zero and the top speed.
    if (target < 0.0f) {
        f32_put(ram, player + PLAYER_TARGET_SPEED, 0.0f);
        target = f32_at(ram, player + PLAYER_TARGET_SPEED);
    }
    float top = f32_at(ram, player + PLAYER_TOP_SPEED);
    fcond_set(ctx, top <= target);
    if (top <= target) f32_put(ram, player + PLAYER_TARGET_SPEED, top);

    uint32_t state = mem_r32(ram, player + PLAYER_STATE);
    if (!(state & 0x8) || (state & 0x40000000u)) {
        float v = f32_at(ram, player + PLAYER_TARGET_SPEED);
        f32_put(ram, player + PLAYER_SPEED_SQUARED, fnan_s(v * v / 25.0f));
    }
    mem_w16(ram, player + PLAYER_UPDATE_FLAGS, (uint16_t)(mem_r16(ram, player + PLAYER_UPDATE_FLAGS) | 0x20));

    if (mem_r32(ram, player + PLAYER_STATUS) & 0x10000000u) {
        mem_w32(ram, frame + 24, player);
        ctx->r[4] = SX32(player);
        ctx->r[5] = (uint64_t)(int64_t)(int8_t)index;  // the index as a signed byte
        call_game(ram, ctx, func_8008F104, frame, 0x80031F20u);
        mem_w32(ram, player + PLAYER_STATUS, mem_r32(ram, player + PLAYER_STATUS) & ~0x10000000u);
    }
    ctx->r[29] = SX32(sp);
    ctx->r[31] = SX32(ra);
}
