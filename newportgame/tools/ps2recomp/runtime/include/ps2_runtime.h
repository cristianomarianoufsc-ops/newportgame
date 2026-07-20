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
static inline uint32_t mem_read32(uint32_t addr) {
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

/* BIOS syscall — called by generated SYSCALL instructions */
void ps2_syscall(PS2Regs* regs, uint32_t code);

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
