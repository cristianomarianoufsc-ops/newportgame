// spu2_stub.cpp — PS2 SPU2 audio stub (placeholder → OpenAL)
//
// The PS2 Sound Processing Unit 2 has:
//   - 48 hardware voices (24 per core)
//   - 2 MB sound RAM (SRAM)
//   - ADSR envelope per voice
//   - Hardware reverb
//
// This stub silently absorbs all SPU2 writes so the game doesn't crash.
// TODO: route to OpenAL for actual audio playback.
//
// SPU2 MMIO lives at 0x1F900000 (IOP address space).
// The EE accesses it via SIF DMA, not direct MMIO — so most audio
// control flows through bios_stub.cpp SIF calls.

#include "../include/ps2_runtime.h"
#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------
// SPU2 Sound RAM (2 MB)
// -----------------------------------------------------------------------
#define SPU2_SRAM_SIZE (2u * 1024u * 1024u)
static uint8_t spu2_sram[SPU2_SRAM_SIZE];

// -----------------------------------------------------------------------
// SPU2 register file (2 × 24 voices + global regs)
// Stored as a flat array indexed by register offset.
// -----------------------------------------------------------------------
#define SPU2_REG_COUNT 0x800
static uint16_t spu2_regs[SPU2_REG_COUNT];

// -----------------------------------------------------------------------
// Voice state (for future OpenAL mapping)
// -----------------------------------------------------------------------
struct SPU2Voice {
    uint16_t vol_l, vol_r;   // Left/right volume
    uint16_t pitch;           // Pitch (0x1000 = 44100 Hz)
    uint32_t start_addr;      // SRAM address of sample start
    uint32_t loop_addr;       // SRAM address of loop point
    uint32_t cur_addr;        // Current read position
    bool     key_on;
    bool     key_off;
    // ADSR
    uint16_t adsr1, adsr2;
    uint32_t adsr_vol;
};
static SPU2Voice voices[48];  // 2 cores × 24 voices

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

extern "C" void spu2_write_reg(uint32_t reg, uint16_t val) {
    if (reg < SPU2_REG_COUNT) spu2_regs[reg] = val;

    // Key-on / Key-off for voices (core 0: 0x1A8/0x1AA, core 1: 0x5A8/0x5AA)
    uint32_t core = (reg >= 0x400) ? 1u : 0u;
    uint32_t local = reg & 0x3FF;

    if (local == 0x1A8) {  // KON_L — key-on voices 0-15
        for (int i = 0; i < 16; i++)
            if (val & (1 << i)) voices[core * 24 + i].key_on = true;
    } else if (local == 0x1AA) {  // KON_H — key-on voices 16-23
        for (int i = 0; i < 8; i++)
            if (val & (1 << i)) voices[core * 24 + 16 + i].key_on = true;
    } else if (local == 0x1AC) {  // KOF_L — key-off voices 0-15
        for (int i = 0; i < 16; i++)
            if (val & (1 << i)) voices[core * 24 + i].key_off = true;
    } else if (local == 0x1AE) {  // KOF_H
        for (int i = 0; i < 8; i++)
            if (val & (1 << i)) voices[core * 24 + 16 + i].key_off = true;
    }
    // TODO: map voice registers (0x000–0x17F) to OpenAL sources
}

extern "C" uint16_t spu2_read_reg(uint32_t reg) {
    if (reg < SPU2_REG_COUNT) return spu2_regs[reg];
    return 0;
}

// DMA transfer from EE RAM into SPU2 SRAM
extern "C" void spu2_dma_write(uint32_t sram_dst, const uint8_t* src, uint32_t size) {
    if (sram_dst >= SPU2_SRAM_SIZE) return;
    uint32_t copy = (sram_dst + size > SPU2_SRAM_SIZE)
                    ? (SPU2_SRAM_SIZE - sram_dst) : size;
    memcpy(spu2_sram + sram_dst, src, copy);
#ifdef PS2_DEBUG
    fprintf(stderr, "[SPU2] DMA write %u bytes → SRAM 0x%05x\n", copy, sram_dst);
#endif
}

// DMA read from SPU2 SRAM into EE RAM
extern "C" void spu2_dma_read(uint32_t sram_src, uint8_t* dst, uint32_t size) {
    if (sram_src >= SPU2_SRAM_SIZE) { memset(dst, 0, size); return; }
    uint32_t copy = (sram_src + size > SPU2_SRAM_SIZE)
                    ? (SPU2_SRAM_SIZE - sram_src) : size;
    memcpy(dst, spu2_sram + sram_src, copy);
}
