// Out-of-line instruction helpers declared in recomp.h: 64-bit multiply/divide and FPU conversions.
#include <math.h>

#include "recomp.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

void op_dmultu(Ctx *c, uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
    uint64_t hi;
    c->lo = _umul128(a, b, &hi);
    c->hi = hi;
#else
    unsigned __int128 p = (unsigned __int128)a * b;
    c->lo = (uint64_t)p;
    c->hi = (uint64_t)(p >> 64);
#endif
}

void op_dmult(Ctx *c, uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
    int64_t hi;
    c->lo = (uint64_t)_mul128((int64_t)a, (int64_t)b, &hi);
    c->hi = (uint64_t)hi;
#else
    __int128 p = (__int128)(int64_t)a * (int64_t)b;
    c->lo = (uint64_t)p;
    c->hi = (uint64_t)((unsigned __int128)p >> 64);
#endif
}

void op_ddiv(Ctx *c, uint64_t a, uint64_t b) {
    int64_t n = (int64_t)a, d = (int64_t)b;
    if (d == 0) {
        c->lo = n >= 0 ? (uint64_t)-1 : 1;
        c->hi = (uint64_t)n;
    } else if (n == INT64_MIN && d == -1) {
        c->lo = (uint64_t)n;
        c->hi = 0;
    } else {
        c->lo = (uint64_t)(n / d);
        c->hi = (uint64_t)(n % d);
    }
}

void op_ddivu(Ctx *c, uint64_t a, uint64_t b) {
    if (b == 0) {
        c->lo = (uint64_t)-1;
        c->hi = a;
    } else {
        c->lo = a / b;
        c->hi = a % b;
    }
}

static double round_mode(double v, int mode) {
    switch (mode) {
    case 0: return nearbyint(v);  // host default is round-half-even, matching the FPU
    case 1: return trunc(v);
    case 2: return ceil(v);
    default: return floor(v);
    }
}

// Out-of-range results follow the R4300 behaviour with the unimplemented-operation trap masked by
// games: the conversion saturates to the "invalid" value 0x7fffffff / 0x7fffffffffffffff.
int32_t fpu_to_w(double v, int mode) {
    double r = round_mode(v, mode);
    if (isnan(r) || r >= 2147483648.0 || r < -2147483648.0) return INT32_MAX;
    return (int32_t)r;
}

int64_t fpu_to_l(double v, int mode) {
    double r = round_mode(v, mode);
    if (isnan(r) || r >= 9223372036854775808.0 || r < -9223372036854775808.0) return INT64_MAX;
    return (int64_t)r;
}
