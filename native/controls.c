// Readable rewrites of game functions (phase 9), replacing their mechanical translation.
// Each one must give the same results as the original: tests/recomp_diff.c compares it with the
// MIPS interpreter and tests/behavior_test.c checks the documented behaviour.
#include "funcs.h"

// --- Analog stick conditioning (FUN_80038534 for X, FUN_800388b0 for Y) ----------------------
// The game reads a stick through a square dead zone: the point where the stick's direction
// crosses the +-12 square is subtracted, so the response starts at zero right outside the dead
// zone instead of jumping, and the result is limited to the +-53 square keeping the direction.

#define STICK_DEAD_ZONE 12
#define STICK_LIMIT 53

typedef struct Stick {
    int x, y;
} Stick;

static int s16(int v) { return (int16_t)v; }

// Shared part of both functions: dead-zone removal and the clamp on x.
static Stick stick_condition(int raw_x, int raw_y) {
    // Where the direction (raw_x, raw_y) leaves the dead-zone square.
    int edge_x = raw_x, edge_y = raw_y;
    if (raw_x > STICK_DEAD_ZONE) {
        edge_y = s16(raw_y * STICK_DEAD_ZONE / raw_x);
        edge_x = STICK_DEAD_ZONE;
    }
    if (edge_x < -STICK_DEAD_ZONE) {
        edge_y = s16(edge_y * STICK_DEAD_ZONE / -edge_x);
        edge_x = -STICK_DEAD_ZONE;
    }
    if (edge_y > STICK_DEAD_ZONE) {
        edge_x = s16(edge_x * STICK_DEAD_ZONE / edge_y);
        edge_y = STICK_DEAD_ZONE;
    }
    if (edge_y < -STICK_DEAD_ZONE) {
        edge_x = s16(edge_x * STICK_DEAD_ZONE / -edge_y);
        edge_y = -STICK_DEAD_ZONE;
    }

    Stick s = {0, 0};
    int outside = raw_x < -STICK_DEAD_ZONE || raw_x > STICK_DEAD_ZONE ||
                  raw_y < -STICK_DEAD_ZONE || raw_y > STICK_DEAD_ZONE;
    if (outside) {
        s.x = s16(raw_x - edge_x);
        s.y = s16(raw_y - edge_y);
    }
    if (s.x > STICK_LIMIT) {
        s.y = s16(s.y * STICK_LIMIT / s.x);
        s.x = STICK_LIMIT;
    }
    if (s.x < -STICK_LIMIT) {
        s.y = s16(s.y * STICK_LIMIT / -s.x);
        s.x = -STICK_LIMIT;
    }
    return s;
}

static int stick_x(int raw_x, int raw_y) {
    Stick s = stick_condition(raw_x, raw_y);
    if (s.y > STICK_LIMIT) s.x = s16(s.x * STICK_LIMIT / s.y);
    if (s.y < -STICK_LIMIT) s.x = s16(s.x * STICK_LIMIT / -s.y);
    return s.x;
}

static int stick_y(int raw_x, int raw_y) {
    Stick s = stick_condition(raw_x, raw_y);
    if (s.y > STICK_LIMIT) s.y = STICK_LIMIT;
    if (s.y < -STICK_LIMIT) s.y = -STICK_LIMIT;
    return s.y;
}

// int FUN_80038534(Controller *c): conditioned stick X. Controller: s16 stick x at +0, y at +2.
void func_80038534(uint8_t *rdram, Ctx *ctx) {
    uint32_t c = (uint32_t)ctx->r[4];
    ctx->r[2] = SX32(stick_x((int16_t)mem_r16(rdram, c), (int16_t)mem_r16(rdram, c + 2)));
}

// int FUN_800388b0(Controller *c): conditioned stick Y.
void func_800388B0(uint8_t *rdram, Ctx *ctx) {
    uint32_t c = (uint32_t)ctx->r[4];
    ctx->r[2] = SX32(stick_y((int16_t)mem_r16(rdram, c), (int16_t)mem_r16(rdram, c + 2)));
}
