// bios_stub.cpp — PS2 IOP / BIOS syscall stubs
//
// The EE (Emotion Engine) communicates with the IOP (I/O Processor) via
// SIF (Serial InterFace) and makes BIOS calls via the SYSCALL instruction.
// This stub intercepts those calls and provides host-side equivalents.
//
// Syscall code is the 20-bit immediate embedded in the SYSCALL instruction
// (bits 25:6 of the raw word), passed here as `code`.
//
// Reference: PS2 SDK EE kernel headers (sceCdSys.h, sifman.h, etc.)

#include "../include/ps2_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <algorithm>

// -----------------------------------------------------------------------
// Syscall frequency counters — printed by dump_syscall_stats()
// -----------------------------------------------------------------------
static std::atomic<uint64_t> g_syscall_total{0};
static std::unordered_map<uint32_t, uint64_t> g_syscall_counts;

extern "C" void dump_syscall_stats(void) {
    fprintf(stderr, "[BIOS] Syscall stats (total=%llu):\n",
            (unsigned long long)g_syscall_total.load());
    // Sort by count descending
    std::vector<std::pair<uint64_t,uint32_t>> v;
    for (auto& kv : g_syscall_counts) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (auto& [cnt, code] : v)
        fprintf(stderr, "  syscall 0x%02x : %llu\n", code, (unsigned long long)cnt);
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static const char* str_from_ram(uint32_t vaddr) {
    uint8_t* p = ps2_mem_ptr(vaddr);
    return p ? (const char*)p : "<invalid>";
}

// -----------------------------------------------------------------------
// EE BIOS syscall codes (incomplete — most common ones)
// -----------------------------------------------------------------------
enum EESyscall : uint32_t {
    SYS_RESET_EE            = 0x01,
    SYS_SET_GS_CRT          = 0x02,  // SetGsCrt / display mode
    SYS_INIT_MAIN_THREAD    = 0x03,
    SYS_INIT_HEAP           = 0x04,
    SYS_END_OF_HEAP         = 0x05,
    SYS_CREATE_THREAD       = 0x20,
    SYS_DELETE_THREAD       = 0x21,
    SYS_START_THREAD        = 0x22,
    SYS_EXIT_THREAD         = 0x23,
    SYS_EXIT_DELETE_THREAD  = 0x24,
    SYS_SLEEP_THREAD        = 0x40,
    SYS_WAKEUP_THREAD       = 0x41,
    SYS_ISIGNAL_SEMA        = 0x45,
    SYS_WAIT_SEMA           = 0x47,
    SYS_CREATE_SEMA         = 0x50,
    SYS_DELETE_SEMA         = 0x51,
    SYS_SIGNAL_SEMA         = 0x52,
    SYS_POLL_SEMA           = 0x53,
    SYS_REFER_SEMA_STATUS   = 0x54,
    SYS_SET_ALARM           = 0x60,
    SYS_RELEASE_ALARM       = 0x62,
    SYS_DI                  = 0x64,  // Disable interrupts
    SYS_EI                  = 0x65,  // Enable interrupts
    SYS_FLUSH_CACHE         = 0x68,
    SYS_GS_GET_IMR          = 0x70,
    SYS_SIF_SET_DMA         = 0x77,
    SYS_SIF_INIT_CMD        = 0x78,
    SYS_SIF_INIT_RPO        = 0x79,
    SYS_SIF_SET_REG         = 0x7A,
    SYS_SIF_GET_REG         = 0x7B,
    SYS_PRINTF              = 0x3C,
    SYS_DPRINTF             = 0x3D,
};

// Simple stub heap tracker (for InitHeap/EndOfHeap)
static uint32_t g_heap_base = 0;
static uint32_t g_heap_size = 0;

// -----------------------------------------------------------------------
// ps2_syscall — called by generated SYSCALL instructions in output.c
// -----------------------------------------------------------------------
extern "C" void ps2_syscall(PS2Regs* regs, uint32_t code) {
    ++g_syscall_total;
    g_syscall_counts[code]++;

    // Registers: a0=$4, a1=$5, a2=$6, a3=$7, v0=$2 (return value)
    uint32_t a0 = (uint32_t)regs->r[4];
    uint32_t a1 = (uint32_t)regs->r[5];
    uint32_t a2 = (uint32_t)regs->r[6];
    uint32_t a3 = (uint32_t)regs->r[7];
    (void)a2; (void)a3;

    switch (code) {

    // ---- Kernel init ----
    case SYS_RESET_EE:
        // a0 = reset flags bitmask — ignored in stub
        regs->r[2] = 0;
        break;

    case SYS_SET_GS_CRT:
        // a0 = interlace, a1 = mode (PAL/NTSC), a2 = field
        // We don't actually change display mode — stub ignores
        regs->r[2] = 0;
        break;

    case SYS_INIT_MAIN_THREAD:
        // a0=gp, a1=stack_base, a2=stack_size, a3=args, stack_param
        // We have no thread system; just record the stack and move on
        regs->r[2] = a1 + a2;   // Return stack top
        break;

    case SYS_INIT_HEAP:
        // a0 = heap base, a1 = heap size (-1 = use all remaining RAM)
        g_heap_base = a0;
        g_heap_size = (a1 == 0xFFFFFFFF) ? (PS2_RAM_SIZE - a0) : a1;
        regs->r[2] = g_heap_base + g_heap_size;
        break;

    case SYS_END_OF_HEAP:
        regs->r[2] = g_heap_base + g_heap_size;
        break;

    // ---- Thread stubs ----
    case SYS_CREATE_THREAD:
        // a0 = ThreadParam* — return a fake thread ID
        regs->r[2] = 1;
        break;

    case SYS_DELETE_THREAD:
        regs->r[2] = 0;
        break;

    case SYS_START_THREAD:
        // a0=thid, a1=arg — in a real impl we'd schedule it
        // Stub: silently succeed; single-threaded execution continues
        regs->r[2] = 0;
        break;

    case SYS_EXIT_THREAD:
    case SYS_EXIT_DELETE_THREAD:
        // Game thread wants to exit — in single-threaded stub just return
        regs->r[2] = 0;
        break;

    case SYS_SLEEP_THREAD:
        // No-op in single-threaded stub
        regs->r[2] = 0;
        break;

    case SYS_WAKEUP_THREAD:
        regs->r[2] = 0;
        break;

    // ---- Semaphore stubs ----
    case SYS_CREATE_SEMA:
        // Return a fake semaphore ID (non-zero = success)
        regs->r[2] = 1;
        break;

    case SYS_DELETE_SEMA:
        regs->r[2] = 0;
        break;

    case SYS_SIGNAL_SEMA:
    case SYS_ISIGNAL_SEMA:
    case SYS_WAIT_SEMA:
    case SYS_POLL_SEMA:
        regs->r[2] = 0;
        break;

    case SYS_REFER_SEMA_STATUS:
        // a0=sema_id, a1=SemaParam* — write stub status
        if (ps2_mem_ptr(a1)) {
            memset(ps2_mem_ptr(a1), 0, 16);   // All zero = idle
        }
        regs->r[2] = 0;
        break;

    // ---- Alarm / timer stubs ----
    case SYS_SET_ALARM:
    case SYS_RELEASE_ALARM:
        regs->r[2] = 0;
        break;

    // ---- Interrupt control ----
    case SYS_DI:
    case SYS_EI:
        // Disable/enable EE interrupts — no-op in stub
        regs->r[2] = 0;
        break;

    // ---- Cache ----
    case SYS_FLUSH_CACHE:
        // a0 = cache type (0=data writeback, 1=icache invalidate)
        // No-op on x86 host
        regs->r[2] = 0;
        break;

    // ---- GS ----
    case SYS_GS_GET_IMR:
        // Return dummy GS interrupt mask
        regs->r[2] = 0xFF00;
        break;

    // ---- SIF / IOP communication ----
    case SYS_SIF_SET_DMA:
    case SYS_SIF_INIT_CMD:
    case SYS_SIF_INIT_RPO:
        // IOP communication stubs — succeed silently
        regs->r[2] = 0;
        break;

    case SYS_SIF_SET_REG:
        // a0 = reg index, a1 = value — stub stores nothing
        regs->r[2] = 0;
        break;

    case SYS_SIF_GET_REG:
        // a0 = reg index — return 0 (IOP not initialised)
        regs->r[2] = 0;
        break;

    // ---- Debug output ----
    case SYS_PRINTF:
    case SYS_DPRINTF: {
        // a0 = format string ptr (PS2 vaddr)
        const char* fmt = str_from_ram(a0);
        // We don't parse format args (they're in PS2 registers/stack).
        // Just print the raw format string so we can see game debug output.
        fprintf(stderr, "[PS2] %s", fmt);
        regs->r[2] = 0;
        break;
    }

    default:
        // Unknown syscall — succeed silently
        // Uncomment to debug: fprintf(stderr, "[BIOS] Unknown syscall 0x%x\n", code);
        regs->r[2] = 0;
        break;
    }
}
