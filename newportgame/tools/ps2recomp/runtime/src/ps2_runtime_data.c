/* ps2_runtime_data.c — Definitions of PS2 RAM/scratchpad arrays
 *
 * These are declared as extern in ps2_runtime.h and referenced by
 * output.c, gs_stub.cpp, bios_stub.cpp, and host_main.cpp.
 *
 * Kept in a separate .c file so the 32 MB zero-initialised block
 * lives in BSS (no disk space cost in the binary).
 */

#include "../include/ps2_runtime.h"

/* 32 MB main RAM — zero-initialised at program start (BSS) */
uint8_t ps2_ram[PS2_RAM_SIZE];

/* 16 KB scratchpad — zero-initialised */
uint8_t ps2_spr[PS2_SPR_SIZE];
