// Native libultra: threads, message queues, events, VI, PI DMA, SP tasks, timers, EEPROM,
// controllers and the Controller Pak, replacing the functions listed in analysis/replaced.json.
//
// Every N64 thread runs on its own Windows fiber with its own Ctx. A host fiber is the
// scheduler: it always resumes the highest-priority ready thread (FIFO among equals), and when
// none is ready it waits for the next external event (vertical retrace). Thread switches happen
// only inside OS calls, which is where libultra games block.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ultra.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"

uint8_t *ultra_rdram;
const uint8_t *ultra_rom;
size_t ultra_rom_size;
UltraStats ultra_stats;
UltraPad ultra_pads[4];

static UltraHost g_host;
static FILE *g_log;

void ultra_log(const char *fmt, ...) {
    if (!g_log) g_log = fopen("mk64_runtime.log", "w");
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// RDRAM accessors (N64 addresses)
#define RD32(a) mem_r32(ultra_rdram, (a))
#define WR32(a, v) mem_w32(ultra_rdram, (a), (v))
#define ARG(c, n) ((uint32_t)(c)->r[4 + (n)])
#define STACK_ARG(c, n) RD32((uint32_t)(c)->r[29] + 16 + 4 * (n))  // 5th argument is n = 0
#define RET(c, v) ((c)->r[2] = SX32((uint32_t)(v)))

// ---------------------------------------------------------------------------------------------
// Threads
enum { T_READY, T_RUNNING, T_WAITING, T_STOPPED, T_CREATED };

typedef struct NThread {
    uint32_t addr;   // OSThread in RDRAM
    int id, pri, state;
    uint32_t entry, wait_queue;
    uint64_t seq;    // FIFO order among equal priorities
    void *fiber;
    Ctx ctx;
} NThread;

#define MAX_THREADS 32
static NThread g_threads[MAX_THREADS];
static int g_thread_count;
static NThread *g_current;      // running game thread, NULL in the host
static void *g_host_fiber;
static uint64_t g_seq;

static NThread *thread_of(uint32_t addr) {
    for (int k = 0; k < g_thread_count; k++)
        if (g_threads[k].addr == addr) return &g_threads[k];
    return NULL;
}

static void make_ready(NThread *t) {
    t->state = T_READY;
    t->wait_queue = 0;
    t->seq = ++g_seq;
}

static void to_host(void) { SwitchToFiber(g_host_fiber); }

// Gives the CPU back to the scheduler if a ready thread outranks the current one.
static void maybe_preempt(void) {
    if (!g_current) return;
    for (int k = 0; k < g_thread_count; k++) {
        NThread *t = &g_threads[k];
        if (t->state == T_READY && t->pri > g_current->pri) {
            make_ready(g_current);
            to_host();
            return;
        }
    }
}

static void WINAPI fiber_main(void *param) {
    NThread *t = param;
    recomp_lookup(t->entry)(ultra_rdram, &t->ctx);
    ultra_log("thread %d (0x%08x) returned", t->id, t->addr);
    t->state = T_STOPPED;
    for (;;) to_host();
}

// ---------------------------------------------------------------------------------------------
// Message queues: OSMesgQueue in RDRAM {mtqueue, fullqueue, validCount, first, msgCount, msg}
#define MQ_VALID 8
#define MQ_FIRST 12
#define MQ_COUNT 16
#define MQ_MSG 20

static void wake_waiters(uint32_t mq) {
    for (int k = 0; k < g_thread_count; k++) {
        NThread *t = &g_threads[k];
        if (t->state == T_WAITING && t->wait_queue == mq) make_ready(t);
    }
}

// Returns 0 or -1 (queue full and not blocking).
static int send_mesg(uint32_t mq, uint32_t msg, int block) {
    for (;;) {
        uint32_t valid = RD32(mq + MQ_VALID), count = RD32(mq + MQ_COUNT);
        if (valid < count) break;
        if (!block || !g_current) return -1;
        g_current->state = T_WAITING;
        g_current->wait_queue = mq;
        to_host();
    }
    uint32_t valid = RD32(mq + MQ_VALID), count = RD32(mq + MQ_COUNT), first = RD32(mq + MQ_FIRST);
    WR32(RD32(mq + MQ_MSG) + 4 * ((first + valid) % count), msg);
    WR32(mq + MQ_VALID, valid + 1);
    wake_waiters(mq);
    return 0;
}

static int recv_mesg(uint32_t mq, uint32_t msg_out, int block) {
    for (;;) {
        if (RD32(mq + MQ_VALID) > 0) break;
        if (!block || !g_current) return -1;
        g_current->state = T_WAITING;
        g_current->wait_queue = mq;
        to_host();
    }
    uint32_t first = RD32(mq + MQ_FIRST), count = RD32(mq + MQ_COUNT);
    if (msg_out) WR32(msg_out, RD32(RD32(mq + MQ_MSG) + 4 * first));
    WR32(mq + MQ_FIRST, (first + 1) % count);
    WR32(mq + MQ_VALID, RD32(mq + MQ_VALID) - 1);
    wake_waiters(mq);  // senders blocked on a full queue
    return 0;
}

// __osEventStateTab: 8 bytes per event {queue, message}, filled by the recompiled osSetEventMesg.
#define EVENT_TABLE 0x80196440u
static uint32_t g_pending_events;  // bit per event, delivered by the scheduler

static void deliver_event(int event) {
    uint32_t q = RD32(EVENT_TABLE + 8 * event);
    if (q) send_mesg(q, RD32(EVENT_TABLE + 8 * event + 4), 0);
}

void ultra_post_event(int event) { g_pending_events |= 1u << event; }

// ---------------------------------------------------------------------------------------------
// Time: CP0 Count runs at half the 93.75 MHz CPU clock.
#define COUNT_HZ 46875000ull
static LARGE_INTEGER g_qpf, g_qpc0;
static int64_t g_time_offset;

// Deterministic mode: time is virtual. A retrace happens as soon as no thread can run, and the
// count advances with retraces plus a fixed step per read, so a run depends only on its inputs.
int ultra_deterministic;
static uint64_t g_virtual_count;
#define VI_PERIOD (1.0 / 59.94)

static uint64_t count64(void) {
    if (ultra_deterministic) return g_virtual_count += 64;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)(now.QuadPart - g_qpc0.QuadPart);
    return ticks / (uint64_t)g_qpf.QuadPart * COUNT_HZ + ticks % (uint64_t)g_qpf.QuadPart * COUNT_HZ / (uint64_t)g_qpf.QuadPart;
}

static double now_seconds(void) {
    if (ultra_deterministic) return (double)ultra_stats.retraces * VI_PERIOD;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_qpc0.QuadPart) / (double)g_qpf.QuadPart;
}

uint32_t recomp_cop0_read(Ctx *ctx, int reg) {
    (void)ctx;
    if (reg == 9) return (uint32_t)count64();
    if (reg == 12) return 0x2000FF01u;  // Status: CU1, all interrupts enabled
    return 0;
}

void recomp_cop0_write(Ctx *ctx, int reg, uint32_t value) {
    (void)ctx;
    (void)reg;
    (void)value;
}

// ---------------------------------------------------------------------------------------------
// VI
static uint32_t g_vi_queue, g_vi_msg, g_vi_retrace = 1;

// ---------------------------------------------------------------------------------------------
// EEPROM (4 Kbit, 64 blocks of 8 bytes), kept in mk64.eep next to the executable.
static uint8_t g_eeprom[512];
static char g_eeprom_path[1024];

static void eeprom_load(void) {
    if (ultra_deterministic) {
        // Replays start from a blank EEPROM and keep their save in the working directory.
        strcpy(g_eeprom_path, "mk64.eep");
        return;
    }
    DWORD n = GetModuleFileNameA(NULL, g_eeprom_path, sizeof g_eeprom_path);
    char *slash = n ? strrchr(g_eeprom_path, '\\') : NULL;
    if (slash) slash[1] = '\0';
    else g_eeprom_path[0] = '\0';
    strncat(g_eeprom_path, "mk64.eep", sizeof g_eeprom_path - strlen(g_eeprom_path) - 1);
    FILE *f = fopen(g_eeprom_path, "rb");
    if (!f) return;  // no save yet: blank EEPROM
    size_t got = fread(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
    ultra_log("EEPROM: %zu bytes from %s", got, g_eeprom_path);
}

#ifdef RECOMP_PROFILE
static uint32_t g_calls[0x100000 / 4];  // per function, code below 0x80100000
static uint32_t g_first[0x100000 / 4];  // retrace of its first call in the window
void recomp_enter(uint32_t addr) {
    uint32_t k = (addr - 0x80000000u) / 4;
    if (k >= sizeof g_calls / sizeof g_calls[0]) return;
    if (!g_calls[k]++) g_first[k] = (uint32_t)ultra_stats.retraces;
}

// MK64_PROFILE=a,b: write the functions called during retraces [a, b) to prof_a_b.txt.
static void profile_retrace(void) {
    static long from = -2, to;
    if (from == -2) {
        const char *e = getenv("MK64_PROFILE");
        from = e ? atol(e) : -1;
        to = e && strchr(e, ',') ? atol(strchr(e, ',') + 1) : 0;
    }
    if (from < 0) return;
    if (ultra_stats.retraces == (uint64_t)from) {
        memset(g_calls, 0, sizeof g_calls);
        memset(g_first, 0, sizeof g_first);
    }
    if (ultra_stats.retraces != (uint64_t)to) return;
    char name[64];
    snprintf(name, sizeof name, "prof_%ld_%ld.txt", from, to);
    FILE *f = fopen(name, "w");
    for (uint32_t k = 0; f && k < sizeof g_calls / sizeof g_calls[0]; k++)
        if (g_calls[k]) fprintf(f, "%08x %u %u\n", 0x80000000u + 4 * k, g_calls[k], g_first[k]);
    if (f) fclose(f);
}
#endif

// ---------------------------------------------------------------------------------------------
// Scheduler (host fiber)
static NThread *pick_ready(void) {
    NThread *best = NULL;
    for (int k = 0; k < g_thread_count; k++) {
        NThread *t = &g_threads[k];
        // Priority 0 is OS_PRIORITY_IDLE: the idle thread only spins, so it is never resumed.
        if (t->state != T_READY || t->pri <= 0) continue;
        if (!best || t->pri > best->pri || (t->pri == best->pri && t->seq < best->seq)) best = t;
    }
    return best;
}

static void scheduler(void) {
    double next_vi = now_seconds() + VI_PERIOD;
    uint64_t vi_counter = 0;
    for (;;) {
        // External events first, so woken threads can run in this pass.
        while (g_pending_events) {
            uint32_t ev = g_pending_events;
            g_pending_events = 0;
            for (int e = 0; e < 32; e++)
                if (ev & (1u << e)) deliver_event(e);
        }
        double now = now_seconds();
        if (ultra_deterministic) {
            // A retrace only when nothing can run, so thread interleaving never depends on speed.
            now = pick_ready() ? -1.0 : next_vi;
        }
        if (now >= next_vi) {
            if (ultra_deterministic) {
                uint64_t target = (uint64_t)((ultra_stats.retraces + 1) * (COUNT_HZ / 59.94));
                if (g_virtual_count < target) g_virtual_count = target;
            }
            next_vi += VI_PERIOD;
            if (now > next_vi + 0.25) next_vi = now + VI_PERIOD;  // do not try to catch up after a stall
            ultra_stats.retraces++;
#ifdef RECOMP_PROFILE
            profile_retrace();
#endif
            {
                // MK64_THREADDUMP=n: log every game thread's state at retrace n (debugging hangs).
                static long dump_at = -2;
                if (dump_at == -2) dump_at = getenv("MK64_THREADDUMP") ? atol(getenv("MK64_THREADDUMP")) : -1;
                if (dump_at >= 0 && ultra_stats.retraces == (uint64_t)dump_at)
                    for (int k = 0; k < g_thread_count; k++) {
                        NThread *t = &g_threads[k];
                        ultra_log("thread %d entry=0x%08x pri=%d state=%d queue=0x%08x (%u/%u msgs)", t->id, t->entry,
                                  t->pri, t->state, t->wait_queue, t->wait_queue ? RD32(t->wait_queue + MQ_VALID) : 0,
                                  t->wait_queue ? RD32(t->wait_queue + MQ_COUNT) : 0);
                    }
            }
            vi_counter++;
            if (ultra_stats.retraces % 300 == 0) {
                static uint64_t last_gfx;
                ultra_log("retrace %llu: %.1f quadros/s", (unsigned long long)ultra_stats.retraces,
                          (double)(ultra_stats.gfx_tasks - last_gfx) / (300.0 / 59.94));
                last_gfx = ultra_stats.gfx_tasks;
            }
            if (g_vi_queue && vi_counter % g_vi_retrace == 0) send_mesg(g_vi_queue, g_vi_msg, 0);
            if (g_host.on_retrace && !g_host.on_retrace(g_host.user)) exit(0);
        }
        NThread *t = pick_ready();
        int ready = 0;
        for (int k = 0; k < g_thread_count; k++) ready += g_threads[k].state == T_READY;
        ultra_stats.threads_ready = ready;
        if (!t) {
            if (ultra_deterministic) continue;
            double wait = next_vi - now_seconds();
            if (wait > 0.0005) Sleep((DWORD)(wait * 1000.0));
            continue;
        }
        g_current = t;
        t->state = T_RUNNING;
        SwitchToFiber(t->fiber);
        g_current = NULL;
    }
}

// ---------------------------------------------------------------------------------------------
// Replaced libultra functions (addresses and evidence in analysis/replaced.json)

void func_800CC0C0(uint8_t *rdram, Ctx *ctx) {  // osInitialize
    (void)rdram;
    (void)ctx;
    WR32(0x80000300u, 1);            // osTvType: NTSC
    WR32(0x80000304u, 0);            // osRomType: cartridge
    WR32(0x80000308u, 0xB0000000u);  // osRomBase
    WR32(0x8000030Cu, 0);            // osResetType: cold reset
    WR32(0x80000318u, 0x400000u);    // osMemSize: 4 MB, no Expansion Pak
    ultra_log("osInitialize");
}

void func_800CBF70(uint8_t *rdram, Ctx *ctx) {  // osCreateThread(t, id, entry, arg, sp, pri)
    (void)rdram;
    uint32_t addr = ARG(ctx, 0);
    NThread *t = thread_of(addr);
    if (!t) {
        if (g_thread_count == MAX_THREADS) recomp_unhandled(addr, "too many threads");
        t = &g_threads[g_thread_count++];
    }
    memset(t, 0, sizeof(*t));
    t->addr = addr;
    t->id = (int)ARG(ctx, 1);
    t->entry = ARG(ctx, 2);
    t->pri = (int)STACK_ARG(ctx, 1);
    t->state = T_CREATED;
    t->ctx.r[4] = SX32(ARG(ctx, 3));
    t->ctx.r[29] = SX32(STACK_ARG(ctx, 0) - 16);
    t->ctx.fcr31 = 0x01000800u;      // libultra default: flush denormals, invalid-op enabled
    t->fiber = CreateFiber(1 << 20, fiber_main, t);
    WR32(addr + 4, (uint32_t)t->pri);
    WR32(addr + 0x14, (uint32_t)t->id);
    ultra_stats.threads = g_thread_count;
    ultra_log("osCreateThread id=%d entry=0x%08x pri=%d sp=0x%08x", t->id, t->entry, t->pri, STACK_ARG(ctx, 0));
}

void func_800CC360(uint8_t *rdram, Ctx *ctx) {  // osStartThread(t)
    (void)rdram;
    NThread *t = thread_of(ARG(ctx, 0));
    if (!t) recomp_unhandled(ARG(ctx, 0), "osStartThread of unknown thread");
    make_ready(t);
    ultra_log("osStartThread id=%d", t->id);
    if (!g_current) scheduler();  // first thread started from boot code: never returns
    maybe_preempt();
}

void func_800CCC80(uint8_t *rdram, Ctx *ctx) {  // osSetThreadPri(t, pri)
    (void)rdram;
    NThread *t = ARG(ctx, 0) ? thread_of(ARG(ctx, 0)) : g_current;
    if (!t) return;
    t->pri = (int)ARG(ctx, 1);
    WR32(t->addr + 4, (uint32_t)t->pri);
    if (g_current && t == g_current) {
        // Lowering ourselves: let anything that now outranks us run (the idle thread drops to 0).
        make_ready(g_current);
        to_host();
    } else {
        maybe_preempt();
    }
}

void func_800CD8C0(uint8_t *rdram, Ctx *ctx) {  // osSendMesg(mq, msg, flags)
    (void)rdram;
    RET(ctx, send_mesg(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2) == 1));
    maybe_preempt();
}

void func_800CD750(uint8_t *rdram, Ctx *ctx) {  // osRecvMesg(mq, &msg, flags)
    (void)rdram;
    RET(ctx, recv_mesg(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2) == 1));
}

void func_800CC4B0(uint8_t *rdram, Ctx *ctx) {  // osCreateViManager(pri)
    (void)rdram;
    ultra_log("osCreateViManager pri=%d", (int)ARG(ctx, 0));
}

void func_800CCAF0(uint8_t *rdram, Ctx *ctx) {  // osCreatePiManager(pri, cmdQ, cmdBuf, cnt)
    (void)rdram;
    ultra_log("osCreatePiManager pri=%d", (int)ARG(ctx, 0));
}

void func_800CC850(uint8_t *rdram, Ctx *ctx) {  // osViSetMode(mode)
    (void)rdram;
    ultra_log("osViSetMode 0x%08x (type %u)", ARG(ctx, 0), RD32(ARG(ctx, 0)) >> 24);
}

void func_800CC8C0(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osViBlack
void func_800CC930(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osViSetSpecialFeatures

void func_800CCD90(uint8_t *rdram, Ctx *ctx) {  // osViSetEvent(mq, msg, retraceCount)
    (void)rdram;
    g_vi_queue = ARG(ctx, 0);
    g_vi_msg = ARG(ctx, 1);
    g_vi_retrace = ARG(ctx, 2) ? ARG(ctx, 2) : 1;
    ultra_log("osViSetEvent queue=0x%08x msg=0x%08x every %u", g_vi_queue, g_vi_msg, g_vi_retrace);
}

void func_800CDA10(uint8_t *rdram, Ctx *ctx) {  // osViSwapBuffer(fb)
    (void)rdram;
    ultra_stats.framebuffer = ARG(ctx, 0);
}

// OSTask (0x40 bytes): type, flags, ucode_boot, ucode_boot_size, ucode, ucode_size, ucode_data,
// ucode_data_size, dram_stack, dram_stack_size, output_buff, output_buff_size, data_ptr,
// data_size, yield_data_ptr, yield_data_size.
static uint32_t g_loaded_task;

void func_800CCF8C(uint8_t *rdram, Ctx *ctx) {  // osSpTaskLoad(task)
    (void)rdram;
    g_loaded_task = ARG(ctx, 0);
}

void func_800CD0EC(uint8_t *rdram, Ctx *ctx) {  // osSpTaskStartGo(task)
    (void)rdram;
    uint32_t task = g_loaded_task ? g_loaded_task : ARG(ctx, 0);
    uint32_t type = RD32(task);
    if (type == 1) ultra_stats.gfx_tasks++;
    else if (type == 2) ultra_stats.audio_tasks++;
    else ultra_stats.other_tasks++;
    uint64_t n = ultra_stats.gfx_tasks + ultra_stats.audio_tasks + ultra_stats.other_tasks;
    if (n <= 12 || n % 600 == 0)
        ultra_log("RSP task #%llu type=%u ucode=0x%08x data=0x%08x size=0x%x", (unsigned long long)n, type,
                  RD32(task + 16), RD32(task + 48), RD32(task + 52));
    if (type == 1 && getenv("MK64_DUMP_DL") && ultra_stats.gfx_tasks <= 3) {
        // Raw display list of the first graphics tasks, to check command encodings.
        uint32_t dl = RD32(task + 48);
        for (int k = 0; k < 160; k++) {
            uint32_t w0 = RD32(dl + 8 * k), w1 = RD32(dl + 8 * k + 4);
            ultra_log("  DL %08x: %08x %08x", dl + 8 * k, w0, w1);
            if ((w0 >> 24) == 0xB8) break;
        }
    }
    if (g_host.on_task) g_host.on_task(g_host.user, task);
    // The task is complete as far as the game can tell: SP done, then (graphics) RDP done.
    ultra_post_event(OS_EVENT_SP);
    if (type == 1) ultra_post_event(OS_EVENT_DP);
}

void func_800CDD40(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osSpTaskYield
void func_800CDD60(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, 0); }  // osSpTaskYielded

// ROM -> RDRAM copy honouring the word-swizzled layout.
static void rom_to_rdram(uint32_t rom_off, uint32_t ram, uint32_t size) {
    for (uint32_t k = 0; k < size; k++) {
        uint32_t src = rom_off + k;
        mem_w8(ultra_rdram, ram + k, src < ultra_rom_size ? ultra_rom[src] : 0);
    }
}

void func_800CDC30(uint8_t *rdram, Ctx *ctx) {  // osPiStartDma(mb, pri, dir, devAddr, vAddr, nbytes, mq)
    (void)rdram;
    uint32_t mb = ARG(ctx, 0), dir = ARG(ctx, 2), dev = ARG(ctx, 3);
    uint32_t vaddr = STACK_ARG(ctx, 0), size = STACK_ARG(ctx, 1), mq = STACK_ARG(ctx, 2);
    if (dir == 0) {
        rom_to_rdram(dev & 0x0FFFFFFFu, vaddr, size);
        ultra_stats.dma_bytes += size;
        ultra_stats.dma_count++;
    } else {
        ultra_log("osPiStartDma write to cart ignored (dev 0x%08x)", dev);
    }
    WR32(mb + 4, mq);
    WR32(mb + 8, vaddr);
    WR32(mb + 12, dev);
    WR32(mb + 16, size);
    if (mq) send_mesg(mq, mb, 0);
    RET(ctx, 0);
}

// Caches do not exist here.
void func_800CDB80(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osInvalDCache
// bzero(ptr, size). Once taken for osInvalICache (it runs before code segments are loaded), but
// its body stores zeros, and the game relies on it, e.g. to empty the object list at each race
// start (0x80165c18, 0x1e140 bytes): without it the 550 slots ran out on the 5th start.
void func_800CDA60(uint8_t *rdram, Ctx *ctx) {
    uint32_t p = ARG(ctx, 0), n = ARG(ctx, 1);
    if ((int32_t)n <= 0) return;
    for (uint32_t k = 0; k < n; k++) mem_w8(rdram, p + k, 0);
}
void func_800CDB00(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osWritebackDCache
void func_800CD890(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }  // osWritebackDCacheAll

void func_800D1AB0(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, 1); }  // __osDisableInt
void func_800D1AD0(uint8_t *rdram, Ctx *ctx) { (void)rdram; (void)ctx; }    // __osRestoreInt
void func_800CE130(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, 0); }  // __osGetActiveQueue

void func_800CDDE0(uint8_t *rdram, Ctx *ctx) {  // osGetTime -> u64 in v0:v1
    (void)rdram;
    uint64_t t = count64() + (uint64_t)g_time_offset;
    ctx->r[2] = SX32((uint32_t)(t >> 32));
    ctx->r[3] = SX32((uint32_t)t);
}

void func_800CE310(uint8_t *rdram, Ctx *ctx) {  // osSetTime(u64 in a0:a1)
    (void)rdram;
    uint64_t t = ((uint64_t)ARG(ctx, 0) << 32) | ARG(ctx, 1);
    g_time_offset = (int64_t)(t - count64());
}

void func_800D11A0(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, (uint32_t)count64()); }  // osGetCount

// Controllers. OSContStatus: {u16 type, u8 status, u8 errno}; OSContPad: {u16 button, s8 x, s8 y, u8 errno}.
#define CONT_TYPE_NORMAL 0x0005
#define CONT_NO_RESPONSE_ERROR 8

void func_800CD130(uint8_t *rdram, Ctx *ctx) {  // osContInit(mq, &bitpattern, status[4])
    (void)rdram;
    uint32_t pattern = ARG(ctx, 1), status = ARG(ctx, 2);
    uint8_t bits = 0;
    for (int k = 0; k < 4; k++) {
        int on = ultra_pads[k].connected;
        bits |= (uint8_t)(on << k);
        mem_w16(ultra_rdram, status + 4 * k, on ? CONT_TYPE_NORMAL : 0);
        mem_w8(ultra_rdram, status + 4 * k + 2, on ? (k == 0 ? 0x01 : 0x02) : 0);  // CONT_CARD_ON
        mem_w8(ultra_rdram, status + 4 * k + 3, on ? 0 : CONT_NO_RESPONSE_ERROR);
    }
    mem_w8(ultra_rdram, pattern, bits);
    mem_w8(ultra_rdram, 0x80196541u, 4);  // __osMaxControllers, read by the libultra Pfs code
    ultra_log("osContInit pattern=0x%x", bits);
    RET(ctx, 0);
}

void func_800CD4F0(uint8_t *rdram, Ctx *ctx) {  // osContStartReadData(mq)
    (void)rdram;
    send_mesg(ARG(ctx, 0), 0, 0);  // the SI transfer completes at once
    RET(ctx, 0);
}

void func_800CD5B4(uint8_t *rdram, Ctx *ctx) {  // osContGetReadData(pads[4])
    (void)rdram;
    uint32_t pads = ARG(ctx, 0);
    for (int k = 0; k < 4; k++) {
        const UltraPad *p = &ultra_pads[k];
        mem_w16(ultra_rdram, pads + 6 * k, p->connected ? p->buttons : 0);
        mem_w8(ultra_rdram, pads + 6 * k + 2, (uint8_t)(p->connected ? p->stick_x : 0));
        mem_w8(ultra_rdram, pads + 6 * k + 3, (uint8_t)(p->connected ? p->stick_y : 0));
        mem_w8(ultra_rdram, pads + 6 * k + 4, p->connected ? 0 : CONT_NO_RESPONSE_ERROR);
    }
}

// EEPROM
static void eeprom_save(void) {
    FILE *f = fopen(g_eeprom_path, "wb");
    if (!f) return;
    fwrite(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
}

void func_800CE340(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, 1); }  // osEepromProbe: EEPROM_TYPE_4K

void func_800D0190(uint8_t *rdram, Ctx *ctx) {  // osEepromLongRead(mq, block, buf, nbytes)
    (void)rdram;
    uint32_t block = ARG(ctx, 1) & 0xFF, buf = ARG(ctx, 2), n = ARG(ctx, 3);
    if (block * 8 + n > sizeof g_eeprom) { RET(ctx, (uint32_t)-1); return; }
    for (uint32_t k = 0; k < n; k++) mem_w8(ultra_rdram, buf + k, g_eeprom[block * 8 + k]);
    RET(ctx, 0);
}

void func_800D0050(uint8_t *rdram, Ctx *ctx) {  // osEepromLongWrite(mq, block, buf, nbytes)
    (void)rdram;
    uint32_t block = ARG(ctx, 1) & 0xFF, buf = ARG(ctx, 2), n = ARG(ctx, 3);
    if (block * 8 + n > sizeof g_eeprom) { RET(ctx, (uint32_t)-1); return; }
    for (uint32_t k = 0; k < n; k++) g_eeprom[block * 8 + k] = mem_r8(ultra_rdram, buf + k);
    eeprom_save();
    RET(ctx, 0);
}

// Controller Pak (32 KB, controller 1), kept in mk64.mpk next to the executable. The game's own
// libultra Pfs code runs on top of the PIF: __osSiRawStartDma moves the 64-byte PIF RAM, and the
// commands in it are carried out here, like the joybus devices would.
#define PAK_SIZE 0x8000
static uint8_t g_pak[PAK_SIZE];
static char g_pak_path[1024];
static uint8_t g_pif[64];

static void pak_save(void) {
    FILE *f = fopen(g_pak_path, "wb");
    if (!f) return;
    fwrite(g_pak, 1, sizeof g_pak, f);
    fclose(f);
}

// A new pak as the console formats it: label, the ID block and its three copies (with their
// checksums), and an inode table with every page free.
static void pak_format(void) {
    memset(g_pak, 0, sizeof g_pak);
    for (int k = 0; k < 32; k++) g_pak[k] = (uint8_t)k;
    g_pak[0] = 0x81;
    uint8_t id[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x05, 0x1A, 0x5F, 0x13, 0, 0, 0, 0, 0, 0, 0, 0,
                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x01, 0x00};
    uint16_t sum = 0, inv = 0;
    for (int k = 0; k < 28; k += 2) {
        uint16_t w = (uint16_t)(id[k] << 8 | id[k + 1]);
        sum = (uint16_t)(sum + w);
        inv = (uint16_t)(inv + (uint16_t)~w);
    }
    id[28] = (uint8_t)(sum >> 8); id[29] = (uint8_t)sum;
    id[30] = (uint8_t)(inv >> 8); id[31] = (uint8_t)inv;
    static const int copies[4] = {0x20, 0x60, 0x80, 0xC0};
    for (int k = 0; k < 4; k++) memcpy(g_pak + copies[k], id, sizeof id);
    for (int page = 1; page <= 2; page++) {  // inode table and its backup
        uint8_t *t = g_pak + page * 0x100;
        for (int e = 5; e < 128; e++) t[2 * e + 1] = 0x03;  // free page
        t[1] = 0x71;                                          // sum of entries 5..127
    }
}

static void pak_load(void) {
    if (ultra_deterministic) {
        strcpy(g_pak_path, "mk64.mpk");  // the working directory (replays start empty: a new pak)
    } else {
        DWORD n = GetModuleFileNameA(NULL, g_pak_path, sizeof g_pak_path);
        char *slash = n ? strrchr(g_pak_path, '\\') : NULL;
        if (slash) slash[1] = '\0';
        else g_pak_path[0] = '\0';
        strncat(g_pak_path, "mk64.mpk", sizeof g_pak_path - strlen(g_pak_path) - 1);
    }
    FILE *f = fopen(g_pak_path, "rb");
    if (f) {
        size_t got = fread(g_pak, 1, sizeof g_pak, f);
        fclose(f);
        ultra_log("Controller Pak: %zu bytes de %s", got, g_pak_path);
        if (got == sizeof g_pak) return;
    }
    pak_format();
}

// Joybus data CRC: polynomial 0x85 over the 32 bytes and one more zero byte.
static uint8_t pak_crc(const uint8_t *data) {
    uint8_t crc = 0;
    for (int i = 0; i <= 32; i++)
        for (int b = 7; b >= 0; b--) {
            uint8_t x = (crc & 0x80) ? 0x85 : 0;
            crc = (uint8_t)(crc << 1);
            if (i < 32 && (data[i] >> b) & 1) crc |= 1;
            crc ^= x;
        }
    return crc;
}

// One joybus command for channel ch: cmd, tx data and room for rx bytes. Returns 0 when nothing
// answers (no controller), which the PIF reports with 0x80 in the rx length.
static int pif_command(int ch, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    if (ch > 3 || !ultra_pads[ch].connected || txn < 1) return 0;
    int pak = ch == 0;
    switch (tx[0]) {
    case 0x00: case 0xFF:  // status: type 0x0005 and pak presence
        if (rxn >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = pak ? 0x01 : 0x02; }
        return 1;
    case 0x01:  // buttons
        if (rxn >= 4) {
            const UltraPad *p = &ultra_pads[ch];
            rx[0] = (uint8_t)(p->buttons >> 8); rx[1] = (uint8_t)p->buttons;
            rx[2] = (uint8_t)p->stick_x; rx[3] = (uint8_t)p->stick_y;
        }
        return 1;
    case 0x02: case 0x03: {  // pak read / write: 16-bit address (low 5 bits are its CRC), 32 bytes
        if (!pak || txn < 3) return 0;
        uint32_t addr = (uint32_t)(tx[1] << 8 | tx[2]) & ~0x1Fu;
        if (tx[0] == 0x02) {
            if (rxn < 33) return 1;
            for (int k = 0; k < 32; k++) rx[k] = addr < PAK_SIZE ? g_pak[addr + k] : 0;
            rx[32] = pak_crc(rx);
        } else {
            if (txn < 35 || rxn < 1) return 1;
            if (addr < PAK_SIZE) {
                memcpy(g_pak + addr, tx + 3, 32);
                pak_save();
            }
            rx[0] = pak_crc(tx + 3);  // 0x8000 and up (bank select, accessories) is ignored
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void pif_run(void) {
    int ch = 0;
    for (int k = 0; k < 63 && ch < 5;) {
        uint8_t txb = g_pif[k];
        if (txb == 0xFE) break;
        if (txb == 0xFF) { k++; continue; }   // padding
        if (txb == 0x00) { k++; ch++; continue; }  // channel skipped
        int txn = txb & 0x3F;
        if (k + 1 >= 63) break;
        int rxn = g_pif[k + 1] & 0x3F;
        int at = k + 2;
        if (at + txn + rxn > 63) break;
        if (!pif_command(ch, g_pif + at, txn, g_pif + at + txn, rxn)) g_pif[k + 1] |= 0x80;
        k = at + txn + rxn;
        ch++;
    }
    g_pif[63] = 0;
}

void func_800D3AA0(uint8_t *rdram, Ctx *ctx) {  // __osSiRawStartDma(direction, pif buffer)
    (void)rdram;
    uint32_t dir = ARG(ctx, 0), buf = ARG(ctx, 1);
    if (dir == 1) {  // OS_WRITE: RDRAM -> PIF RAM, which runs the commands
        for (int k = 0; k < 64; k++) g_pif[k] = mem_r8(ultra_rdram, buf + (uint32_t)k);
        static int logged;
        if (getenv("MK64_PIFLOG") && ultra_stats.retraces >= (uint64_t)atol(getenv("MK64_PIFLOG")) && logged++ < 60) {
            char line[200];
            for (int k = 0; k < 64; k++) snprintf(line + 2 * k, 3, "%02x", g_pif[k]);
            ultra_log("PIF in  %s", line);
        }
        if (g_pif[63] & 1) pif_run();
        if (getenv("MK64_PIFLOG") && logged && logged <= 60) {
            char line[200];
            for (int k = 0; k < 64; k++) snprintf(line + 2 * k, 3, "%02x", g_pif[k]);
            ultra_log("PIF out %s", line);
        }
    } else {  // OS_READ: PIF RAM -> RDRAM
        for (int k = 0; k < 64; k++) mem_w8(ultra_rdram, buf + (uint32_t)k, g_pif[k]);
    }
    ultra_post_event(OS_EVENT_SI);  // the SI interrupt that ends the DMA
    RET(ctx, 0);
}

void func_800D7B70(uint8_t *rdram, Ctx *ctx) { (void)rdram; RET(ctx, 0); }  // __osSiDeviceBusy

// Audio interface. The NTSC VI clock drives the AI DAC.
#define VI_NTSC_CLOCK 48681812u
void func_800D0F80(uint8_t *rdram, Ctx *ctx) {  // osAiSetFrequency(freq)
    (void)rdram;
    uint32_t freq = ARG(ctx, 0);
    uint32_t dac = freq ? (uint32_t)((double)VI_NTSC_CLOCK / freq + 0.5) : 0;
    if (dac < 132) { RET(ctx, (uint32_t)-1); return; }
    ultra_stats.ai_frequency = VI_NTSC_CLOCK / dac;
    ultra_log("osAiSetFrequency %u -> %u Hz", freq, ultra_stats.ai_frequency);
    RET(ctx, ultra_stats.ai_frequency);
}
void func_800D10E0(uint8_t *rdram, Ctx *ctx) {  // osAiGetLength: bytes not yet played
    (void)rdram;
    RET(ctx, g_host.ai_queued ? g_host.ai_queued(g_host.user) : 0);
}

void func_800D10F0(uint8_t *rdram, Ctx *ctx) {  // osAiSetNextBuffer(ptr, size)
    (void)rdram;
    if (g_host.on_ai_buffer) g_host.on_ai_buffer(g_host.user, ARG(ctx, 0), ARG(ctx, 1), ultra_stats.ai_frequency);
    RET(ctx, 0);
}

// ---------------------------------------------------------------------------------------------
void ultra_run(uint8_t *rdram, const uint8_t *rom, size_t rom_size, const UltraHost *host) {
    ultra_rdram = rdram;
    ultra_rom = rom;
    ultra_rom_size = rom_size;
    if (host) g_host = *host;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    eeprom_load();
    pak_load();

    // IPL3 (CIC-6102) copies 1 MB from ROM 0x1000 to the entry point, then jumps there.
    rom_to_rdram(0x1000, 0x80000400u, 0x100000);
    g_host_fiber = ConvertThreadToFiber(NULL);
    static Ctx boot;
    memset(&boot, 0, sizeof boot);
    boot.fcr31 = 0x01000800u;
    ultra_log("boot: entry 0x80000400");
    func_80000400(rdram, &boot);
    recomp_unhandled(0x80000400u, "boot code returned");
}
