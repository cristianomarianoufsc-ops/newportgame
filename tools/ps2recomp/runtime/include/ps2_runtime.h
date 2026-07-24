#pragma once
/* ps2_runtime.h — PS2 recompiler runtime interface
 *
 * C-compatible header: included by both the generated C (output.c) and
 * the C++ runtime (gs_stub.cpp, bios_stub.cpp, host_main.cpp).
 *
 * Layout:
 *   - PS2Regs struct           (register file)
 *   - ps2_ram / ps2_spr        (main RAM + scratchpad — defined in ps2_runtime_data.c)
 *   - Inline memory accessors  (mem_read32, mem_write32, etc.)
 *   - Function declarations    (gs_write_reg, ps2_syscall, ps2_game_start)
 */

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * PS2 memory sizes
 * ----------------------------------------------------------------------- */
#define PS2_RAM_SIZE   (32u * 1024u * 1024u)   /* 32 MB main RAM           */
#define PS2_SPR_SIZE   (16u * 1024u)            /* 16 KB scratchpad         */
#define PS2_VADDR_MASK 0x1FFFFFFFu              /* Strip kseg cache bits    */

/* -----------------------------------------------------------------------
 * Register file
 * EE (Emotion Engine) MIPS R5900 — 32×64-bit GPRs.
 * The upper 64 bits of each 128-bit register are not modelled here.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint64_t r[32];   /* GPRs: r[0] is always 0, enforced per-instruction  */
    uint64_t hi, lo;  /* HI/LO — result of MULT/DIV                        */
    uint64_t hi1, lo1;/* Second HI/LO pipeline (EE extension)              */
    uint32_t pc;      /* Program counter (tracked optionally)              */
    uint32_t sa;      /* Shift-amount register (EE extension)              */
    /* FPU (COP1) — scalar float registers */
    float    f[32];   /* Floating-point registers                          */
    uint32_t cop0[32];/* COP0 system registers ([12]=Status; EIE=bit16)    */
    uint32_t fcr31;   /* FPU control/status register                       */
    /* COP2 / VU0 — 32 × 128-bit vector float registers.
     * Stored as two uint64_t halves: vf[reg][0] = lo64, vf[reg][1] = hi64.
     * Most boot-path code only uses the lo half (xyzw scalar ops). */
    uint64_t vf[32][2];
} PS2Regs;

/* -----------------------------------------------------------------------
 * Memory — arrays defined in ps2_runtime_data.c
 * ----------------------------------------------------------------------- */
extern uint8_t ps2_ram[PS2_RAM_SIZE];
extern uint8_t ps2_spr[PS2_SPR_SIZE];

/* Map a virtual address to a host pointer (NULL if unmapped / I/O) */
static inline uint8_t* ps2_mem_ptr(uint32_t addr) {
    addr &= PS2_VADDR_MASK;
    if (addr < PS2_RAM_SIZE)
        return ps2_ram + addr;
    if (addr >= 0x70000000u && addr < 0x70000000u + PS2_SPR_SIZE)
        return ps2_spr + (addr - 0x70000000u);
    return (uint8_t*)0;   /* I/O space or unmapped */
}

/* -----------------------------------------------------------------------
 * Memory accessors — inline for speed
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Hardware I/O register stubs (addresses 0x1000_0000 - 0x1FFF_FFFF
 * after masking).  The real hardware is not present on the host; we
 * return values that unblock common polling loops so the game can
 * proceed past IOP/DMA init without hanging forever.
 * ----------------------------------------------------------------------- */
static inline uint32_t hw_read32(uint32_t masked) {
    switch (masked) {
    /* INTC_STAT (0x1000f000): all interrupt sources appear asserted so any
     * "wait for IRQ X" loop exits on the first poll.  Writing clears bits
     * (handled in hw_write32 below — ignored for now). */
    case 0x1000f000u: return 0xFFFFFFFFu;
    /* INTC_MASK: all channels unmasked */
    case 0x1000f010u: return 0xFFFFu;
    /* DMAC D0-D9 CHCR (channel control) — bit 8 (STR) clear = idle/done.
     * The game polls CHCR.STR waiting for DMA completion; returning 0
     * means "transfer finished" immediately. */
    case 0x10008000u: /* D0 CHCR */ return 0u;
    case 0x10009000u: /* D1 CHCR */ return 0u;
    case 0x1000a000u: /* D2 CHCR */ return 0u;
    case 0x1000b000u: /* D3 CHCR */ return 0u;
    case 0x1000b400u: /* D4 CHCR */ return 0u;
    case 0x1000c000u: /* D5 CHCR */ return 0u;
    case 0x1000c400u: /* D6 CHCR */ return 0u;
    case 0x1000d000u: /* D7 CHCR (SPR to/from) */ return 0u;
    case 0x1000d400u: /* D8 CHCR (SIF0) */ return 0u;
    case 0x1000e000u: /* D9 CHCR (SIF1) */ return 0u;
    /* DMAC STAT — all channels idle, no pending transfer */
    case 0x1000e010u: return 0u;
    /* SIF bus status (0x1000f230): BOOTEND and SIF1 ready bits set */
    case 0x1000f230u: return 0x100u;  /* SIF BOOTEND */
    /* GS privileged registers — return safe defaults */
    case 0x12000000u: return 0u; /* PMODE */
    case 0x12000010u: return 0u; /* SMODE1 */
    case 0x12000020u: return 0u; /* SMODE2 */
    case 0x12001000u: return 0u; /* CSR — VSync bits; 0 = safe */
    default:          return 0u; /* unknown I/O — succeed silently */
    }
}

static inline uint32_t mem_read32(uint32_t addr) {
    uint32_t masked = addr & PS2_VADDR_MASK;
    /* I/O space: 0x10000000 – 0x13FFFFFF (after masking) */
    if (masked >= 0x10000000u && masked < 0x14000000u)
        return hw_read32(masked);
    uint32_t v = 0;
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(&v, p, 4);
    return v;
}
static inline uint16_t mem_read16(uint32_t addr) {
    uint16_t v = 0;
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(&v, p, 2);
    return v;
}
static inline uint8_t mem_read8(uint32_t addr) {
    uint8_t* p = ps2_mem_ptr(addr);
    return p ? *p : 0;
}
static inline void mem_write32(uint32_t addr, uint32_t v) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(p, &v, 4);
}
static inline void mem_write16(uint32_t addr, uint16_t v) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(p, &v, 2);
}
static inline void mem_write8(uint32_t addr, uint8_t v) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) *p = v;
}
static inline uint64_t mem_read64(uint32_t addr) {
    uint64_t v = 0;
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(&v, p, 8);
    return v;
}
static inline void mem_write64(uint32_t addr, uint64_t v) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) memcpy(p, &v, 8);
}
/* 128-bit read/write — used by COP2 LQC2 / SQC2 instructions.
 * Splits the 128-bit value into two 64-bit halves (lo, hi). */
static inline void mem_read128(uint32_t addr, uint64_t* lo, uint64_t* hi) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) {
        memcpy(lo, p,     8);
        memcpy(hi, p + 8, 8);
    } else {
        *lo = *hi = 0;
    }
}
static inline void mem_write128(uint32_t addr, uint64_t lo, uint64_t hi) {
    uint8_t* p = ps2_mem_ptr(addr);
    if (p) {
        memcpy(p,     &lo, 8);
        memcpy(p + 8, &hi, 8);
    }
}

/* -----------------------------------------------------------------------
 * Runtime entry points (implemented in runtime/src/)
 * ----------------------------------------------------------------------- */

/* GS register write — called by generated MIPS code */
void gs_write_reg(uint64_t reg, uint64_t value);

/* Loud diagnostics for jumps into holes of the dispatch table
 * (rate-limited; implemented in ps2_runtime_data.c) */
void ps2_report_unknown_dispatch(uint32_t addr, uint32_t from_pc);

/* BIOS syscall — called by generated SYSCALL instructions */
void ps2_syscall(PS2Regs* regs, uint32_t code);

/* PC sampler hook — points at the active PS2Regs.pc while the game runs.
 * Set by ps2_game_start(); sampled by the host watchdog to locate guest-code
 * spin loops (dump via dump_pc_samples()). */
extern volatile uint32_t* ps2_active_pc;
void dump_pc_samples(void);
void start_pc_sampler(void);

/* SPU2 register I/O — called by generated MIPS code */
void     spu2_write_reg(uint32_t reg, uint16_t val);
uint16_t spu2_read_reg(uint32_t reg);

/* Game entry point — generated by the recompiler in output.c */
void ps2_game_start(void);

/* Dynamic dispatch — defined as static inside output_runtime.c.
 * Do NOT declare here (extern vs static conflict). */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* -----------------------------------------------------------------------
 * Headless mode flag
 * Set to 1 before calling ps2_game_start() to suppress all GL/SDL calls.
 * Defined in gs_stub.cpp, read by host_main.cpp and gs_stub.cpp.
 * ----------------------------------------------------------------------- */
#ifdef __cplusplus
extern bool g_headless;
#endif
