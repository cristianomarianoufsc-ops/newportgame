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

// -----------------------------------------------------------------------
// Syscall trace tool — set PS2_TRACE_SYSCALLS=N to print the first N calls
// of EACH syscall code with args (a0-a3) and return value (v0). Great for
// diagnosing spin loops and unknown syscalls without a debugger.
// -----------------------------------------------------------------------
static uint32_t g_trace_limit = []() -> uint32_t {
    const char* e = getenv("PS2_TRACE_SYSCALLS");
    return e ? (uint32_t)strtoul(e, nullptr, 0) : 0;
}();

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
// Public dispatcher from the recompiled output — lets stubs invoke guest
// code (thread entries, callbacks) synchronously.
extern "C" void ps2_call(uint32_t addr, PS2Regs* regs);

// Minimal thread table for CreateThread/StartThread (no scheduler:
// StartThread runs the entry to completion synchronously).
#define MAX_THREADS 32
struct GuestThread {
    uint32_t func = 0, stack = 0, stack_size = 0, gp = 0;
    bool valid = false;
};
static GuestThread g_threads[MAX_THREADS];
static uint32_t    g_next_tid = 2;   // 1 is reserved for the main thread

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
// NOTE: this table previously used WRONG numbers (0x3c was treated as
// printf when it is really SetupThread — so the game's sp stayed 0 and
// every stack pointer went negative; 0x3d is SetupHeap, so the heap was
// never initialised and malloc spun on an empty free list). These values
// now follow the real EE kernel table (ps2sdk syscallno).
enum EESyscall : uint32_t {
    SYS_RESET_EE            = 0x01,
    SYS_SET_GS_CRT          = 0x02,  // SetGsCrt / display mode
    SYS_EXIT                = 0x04,
    SYS_LOAD_EXEC_PS2       = 0x06,
    SYS_ADD_INTC_HANDLER    = 0x10,
    SYS_REMOVE_INTC_HANDLER = 0x11,
    SYS_ADD_DMAC_HANDLER    = 0x12,
    SYS_REMOVE_DMAC_HANDLER = 0x13,
    SYS_ENABLE_INTC         = 0x14,
    SYS_DISABLE_INTC        = 0x15,
    SYS_ENABLE_DMAC         = 0x16,
    SYS_DISABLE_DMAC        = 0x17,
    SYS_CREATE_THREAD       = 0x20,
    SYS_DELETE_THREAD       = 0x21,
    SYS_START_THREAD        = 0x22,
    SYS_EXIT_THREAD         = 0x23,
    SYS_EXIT_DELETE_THREAD  = 0x24,
    SYS_TERMINATE_THREAD    = 0x25,
    SYS_CHANGE_THREAD_PRIO  = 0x29,
    SYS_ICHANGE_THREAD_PRIO = 0x2A,
    SYS_ROTATE_TREADY_QUEUE = 0x2B,
    SYS_GET_THREAD_ID       = 0x2F,
    SYS_REFER_THREAD_STATUS = 0x30,
    SYS_SLEEP_THREAD        = 0x32,
    SYS_WAKEUP_THREAD       = 0x33,
    SYS_IWAKEUP_THREAD      = 0x34,
    SYS_CANCEL_WAKEUP       = 0x35,
    SYS_SETUP_THREAD        = 0x3C,  // RFU060: returns stack top -> sp
    SYS_SETUP_HEAP          = 0x3D,  // RFU061: returns heap end
    SYS_END_OF_HEAP         = 0x3E,
    SYS_CREATE_SEMA         = 0x40,
    SYS_DELETE_SEMA         = 0x41,
    SYS_SIGNAL_SEMA         = 0x42,
    SYS_ISIGNAL_SEMA        = 0x43,
    SYS_WAIT_SEMA           = 0x44,
    SYS_POLL_SEMA           = 0x45,
    SYS_IPOLL_SEMA          = 0x46,
    SYS_REFER_SEMA_STATUS   = 0x47,
    SYS_IREFER_SEMA_STATUS  = 0x48,
    SYS_SET_ALARM           = 0x4A,  // (also iWakeupThread-adjacent codes)
    SYS_RELEASE_ALARM       = 0x4B,
    SYS_FLUSH_CACHE         = 0x64,
    SYS_GS_GET_IMR          = 0x70,
    SYS_GS_PUT_IMR          = 0x71,
    SYS_SET_VSYNC_FLAG      = 0x72,
    SYS_SET_SYSCALL         = 0x74,
    SYS_SIF_DMA_STAT        = 0x76,
    SYS_SIF_SET_DMA         = 0x77,
    SYS_SIF_SET_DCHAIN      = 0x78,
    SYS_SIF_SET_REG         = 0x79,
    SYS_SIF_GET_REG         = 0x7A,
    SYS_DECI2_CALL          = 0x7C,
    SYS_PS_MODE             = 0x7D,
    SYS_MACHINE_TYPE        = 0x7E,
    SYS_GET_MEMORY_SIZE     = 0x7F,
    // SIF DMA buffer submission — returns pointer into DMA ring buffer.
    // Used in a two-cursor merge loop: the game calls it twice, then loops
    // advancing whichever cursor is behind until (s3-524) == (s2-360).
    // We return alternating values that satisfy this immediately.
    SYS_SIF_SET_DMA2        = 0x83,
};

// -----------------------------------------------------------------------
// Fake SIF DMA buffer — two consecutive calls must return values V1, V2
// such that (V1 - 524) == (V2 - 360), i.e. V1 - V2 == 164.
// We allocate a small region in safe PS2 RAM (0x500000) and alternate.
// -----------------------------------------------------------------------
static const uint32_t SIF_DMA_BASE = 0x00500000u;  // safe area in PS2 RAM
// First  call: SIF_DMA_BASE + 524  → s3, then s3-524 = SIF_DMA_BASE
// Second call: SIF_DMA_BASE + 360  → s2, then s2-360 = SIF_DMA_BASE
// Condition s3-524 == s2-360 satisfied → loop exits immediately.
static uint32_t g_sif_dma_call = 0;

// Simple stub heap tracker (for InitHeap/EndOfHeap)
static uint32_t g_heap_base = 0;
static uint32_t g_heap_size = 0;

// -----------------------------------------------------------------------
// ps2_syscall — called by generated SYSCALL instructions in output.c
// -----------------------------------------------------------------------
extern "C" void ps2_syscall(PS2Regs* regs, uint32_t /*immediate_unused*/) {
    // PS2 EE convention: real syscall number is in $v1 (r3).
    // Games use either positive r3=N or negative r3=-N (two's complement).
    int32_t r3 = (int32_t)(uint32_t)regs->r[3];
    uint32_t code = (r3 < 0) ? (uint32_t)(-r3) : (uint32_t)r3;

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

    case SYS_SETUP_THREAD:
        // RFU060: a0=gp, a1=stack_base (-1 = auto), a2=stack_size,
        // a3=args, t0=root func. Returns the initial sp (stack top).
        if (a1 == 0xFFFFFFFF) {
            regs->r[2] = 0x01FFFF80u;                     // top of 32MB RAM
        } else {
            regs->r[2] = a1 + a2 - 0x10;
        }
        fprintf(stderr, "[BIOS] SetupThread gp=0x%x stack=0x%x+0x%x -> sp=0x%x\n",
                a0, a1, a2, (uint32_t)regs->r[2]);

        // Inicializa sentinela da fila de threads (func_13fab8).
        // mem[0x2CBBB0] é o ponteiro da cabeça da lista ligada de threads.
        // Em BSS = 0; a travessia checa: if (head == 0x2CBBB0) → lista vazia.
        // Com head=0 o loop nunca encontra o sentinela → spin infinito.
        // Apontando a cabeça para si mesma o check passa imediatamente.
        if (mem_read32(0x2CBBB0u) == 0) {
            mem_write32(0x2CBBB0u, 0x2CBBB0u);
            fprintf(stderr, "[BIOS] Thread-queue sentinel init: mem[0x2CBBB0]=0x2CBBB0\n");
        }
        break;

    case SYS_SETUP_HEAP:
        // RFU061: a0 = heap base, a1 = heap size (-1 = all remaining RAM).
        g_heap_base = a0;
        g_heap_size = (a1 == 0xFFFFFFFF) ? (PS2_RAM_SIZE - a0) : a1;
        regs->r[2] = g_heap_base + g_heap_size;
        fprintf(stderr, "[BIOS] SetupHeap base=0x%x size=0x%x -> end=0x%x\n",
                g_heap_base, g_heap_size, (uint32_t)regs->r[2]);
        break;

    case SYS_END_OF_HEAP:
        regs->r[2] = g_heap_base + g_heap_size;
        break;

    // ---- Thread stubs ----
    case SYS_CREATE_THREAD: {
        // a0 = ThreadParam* {status, func, stack, stack_size, gp, prio}
        // Record it so StartThread can run the entry synchronously.
        uint32_t tid = g_next_tid < MAX_THREADS ? g_next_tid++ : 0;
        if (tid) {
            g_threads[tid].func       = mem_read32(a0 + 4);
            g_threads[tid].stack      = mem_read32(a0 + 8);
            g_threads[tid].stack_size = mem_read32(a0 + 12);
            g_threads[tid].gp         = mem_read32(a0 + 16);
            g_threads[tid].valid      = true;
            fprintf(stderr, "[BIOS] CreateThread tid=%u entry=0x%08x stack=0x%08x+0x%x\n",
                    tid, g_threads[tid].func, g_threads[tid].stack,
                    g_threads[tid].stack_size);
        }
        regs->r[2] = tid ? tid : (uint32_t)-1;
        break;
    }

    case SYS_DELETE_THREAD:
        regs->r[2] = 0;
        break;

    case SYS_START_THREAD: {
        // a0=thid, a1=arg — no scheduler: run the thread entry to
        // completion synchronously on a fresh register file. This is what
        // lets init threads (heap/free-list setup etc.) actually run.
        uint32_t tid = a0;
        if (tid < MAX_THREADS && g_threads[tid].valid && g_threads[tid].func) {
            PS2Regs t = {};
            t.cop0[12] = 0x10001u;                       // Status EIE|IE
            t.r[4]  = a1;                                // a0 = arg
            t.r[28] = g_threads[tid].gp;                 // gp
            t.r[29] = g_threads[tid].stack + g_threads[tid].stack_size - 16;
            t.r[31] = 0;                                 // ra: return = exit
            fprintf(stderr, "[BIOS] StartThread tid=%u -> running entry 0x%08x synchronously\n",
                    tid, g_threads[tid].func);
            ps2_call(g_threads[tid].func, &t);
            fprintf(stderr, "[BIOS] StartThread tid=%u: entry returned\n", tid);
        } else {
            fprintf(stderr, "[BIOS] StartThread: unknown tid=%u (no-op)\n", tid);
        }
        regs->r[2] = 0;
        break;
    }

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
    case SYS_IWAKEUP_THREAD:
    case SYS_CANCEL_WAKEUP:
    case SYS_CHANGE_THREAD_PRIO:
    case SYS_ICHANGE_THREAD_PRIO:
    case SYS_ROTATE_TREADY_QUEUE:
        regs->r[2] = 0;
        break;

    case SYS_GET_THREAD_ID:
        regs->r[2] = 1;   // main thread
        break;

    case SYS_TERMINATE_THREAD:
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
    case SYS_IPOLL_SEMA:
        regs->r[2] = 0;
        break;

    case SYS_IREFER_SEMA_STATUS:
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

    // ---- Interrupt / DMAC handler control ----
    case SYS_ADD_INTC_HANDLER:
    case SYS_ADD_DMAC_HANDLER:
        // a0=cause, a1=handler — return a fake handler id
        regs->r[2] = 1;
        break;
    case SYS_REMOVE_INTC_HANDLER:
    case SYS_REMOVE_DMAC_HANDLER:
    case SYS_ENABLE_INTC:
    case SYS_DISABLE_INTC:
    case SYS_ENABLE_DMAC:
    case SYS_DISABLE_DMAC:
        regs->r[2] = 1;
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

    // ---- GS / vsync ----
    case SYS_GS_PUT_IMR:
    case SYS_SET_VSYNC_FLAG:
        regs->r[2] = 0;
        break;

    // ---- SIF / IOP communication ----
    case SYS_SIF_SET_DMA:
    case SYS_SIF_SET_DCHAIN:
    case SYS_SIF_DMA_STAT:
        // IOP communication stubs — succeed silently
        regs->r[2] = 0;
        break;

    case SYS_SIF_SET_REG:
        // a0 = reg index, a1 = value — stub stores nothing
        regs->r[2] = 0;
        break;

    case SYS_SIF_GET_REG:
        // a0 = reg index / bitmask.
        // GoW boot polls this in two patterns:
        //   1. a0=0x80000000 → "IOP initialised?" → check result != 0
        //   2. a0=4          → bitmask poll at L_296710 → check result & 0x20000 != 0
        // Returning 0x20000 satisfies both (non-zero AND bit-17 set).
        regs->r[2] = 0x20000u;
        break;

    // ---- SIF DMA buffer allocation (0x83) ----
    // This syscall returns a pointer into the EE-side SIF DMA ring buffer.
    // GoW's init (func_299390 @ 0x299390) calls it in a two-cursor merge loop:
    //
    //   s3 = SIF_SET_DMA2(a0, a1, a2=0x2993B8)   // "524" call site
    //   s2 = SIF_SET_DMA2(a0, a1, a2=0x299380)   // "360" call site
    //   s1 = s3 - 524
    //   s0 = s2 - 360
    //   while (s1 != s0) { advance whichever cursor is behind via more calls }
    //
    // LESSON (1.79B-call spin): a global alternating counter breaks if ANY
    // extra 0x83 call happens first — parity inverts and s1/s0 sit 164 bytes
    // apart forever. The two call sites are distinguishable by a2 (r6):
    //   a2 = 0x2A0000 - 27848 = 0x299338  → caller subtracts 524
    //   a2 = 0x2A0000 - 27904 = 0x299300  → caller subtracts 360
    // Return deterministically per call site so both cursors always land on
    // SIF_DMA_BASE regardless of call order or count.
    case SYS_SIF_SET_DMA2: {
        g_sif_dma_call++;
        switch (a2) {
        case 0x299338u: regs->r[2] = SIF_DMA_BASE + 524u; break;
        case 0x299300u: regs->r[2] = SIF_DMA_BASE + 360u; break;
        default:
            // Unknown call site — return the base itself and log once so a
            // new spin shows up in the trace instead of hiding.
            if (g_sif_dma_call < 4)
                fprintf(stderr, "[BIOS] 0x83 unknown a2=0x%x (a0=0x%x a1=0x%x)\n",
                        a2, a0, a1);
            regs->r[2] = SIF_DMA_BASE;
            break;
        }
        break;
    }

    // ---- Debug / misc ----
    case SYS_DECI2_CALL:
        regs->r[2] = 1;
        break;
    case SYS_PS_MODE:
    case SYS_MACHINE_TYPE:
        regs->r[2] = 0;
        break;
    case SYS_GET_MEMORY_SIZE:
        regs->r[2] = PS2_RAM_SIZE;
        break;

    default:
        // Unknown syscall — succeed silently (traced below when enabled)
        regs->r[2] = 0;
        break;
    }

    // Trace tool: log first N calls of each code (env PS2_TRACE_SYSCALLS=N)
    if (g_trace_limit && g_syscall_counts[code] <= g_trace_limit) {
        fprintf(stderr,
                "[TRACE] syscall 0x%02x #%llu  a0=0x%x a1=0x%x a2=0x%x a3=0x%x -> v0=0x%x\n",
                code, (unsigned long long)g_syscall_counts[code],
                a0, a1, a2, a3, (uint32_t)regs->r[2]);
    }
}
