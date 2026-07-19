// host_main.cpp — SDL2 host + ELF loader + game loop
//
// This is the real main() for the recompiled PS2 game.
// Flow:
//   1. Parse args (optional ELF path)
//   2. Load ELF segments into ps2_ram
//   3. Initialise SDL2 window + OpenGL 3.3 context
//   4. Init GS stub (shader, VAO/VBO)
//   5. Set frame callback so gs_stub can swap buffers without linking SDL
//   6. Call ps2_game_start() — runs forever, game loop inside
//      GS_REG_FINISH writes trigger gs_frame_end() → frame callback
//
// Build target: MX Linux / Debian (libsdl2-dev, libglew-dev, libopenal-dev)

#include "../include/ps2_runtime.h"

#include <SDL2/SDL.h>
#include <GL/glew.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <atomic>

// Declared in gs_stub.cpp
extern "C" {
    void gs_gl_init(int w, int h);
    void gs_set_viewport(int w, int h);
    void gs_frame_begin(void);
    void gs_frame_end(void);
}
void gs_set_frame_callback(void (*cb)(void));

// -----------------------------------------------------------------------
// ELF32 loader — loads PT_LOAD segments into ps2_ram
// -----------------------------------------------------------------------
#pragma pack(push, 1)
struct Elf32Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32Phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
};
#pragma pack(pop)

static bool load_elf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[HOST] Cannot open ELF: %s\n", path); return false; }

    Elf32Ehdr hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        memcmp(hdr.e_ident, "\x7f""ELF", 4) != 0) {
        fprintf(stderr, "[HOST] Not a valid ELF file: %s\n", path);
        fclose(f); return false;
    }
    if (hdr.e_machine != 8) {  // EM_MIPS = 8
        fprintf(stderr, "[HOST] Warning: ELF machine type 0x%x (expected MIPS)\n",
                hdr.e_machine);
    }

    fprintf(stderr, "[HOST] ELF entry=0x%08x  phnum=%u\n",
            hdr.e_entry, hdr.e_phnum);

    for (uint16_t i = 0; i < hdr.e_phnum; i++) {
        if (fseek(f, hdr.e_phoff + i * sizeof(Elf32Phdr), SEEK_SET) != 0) break;
        Elf32Phdr ph;
        if (fread(&ph, sizeof(ph), 1, f) != 1) break;
        if (ph.p_type != 1 /* PT_LOAD */ || ph.p_filesz == 0) continue;

        // Strip EE kseg bits to get RAM offset
        uint32_t vaddr = ph.p_vaddr & PS2_VADDR_MASK;
        if (vaddr + ph.p_filesz > PS2_RAM_SIZE) {
            fprintf(stderr, "[HOST]   Skipping segment: vaddr=0x%08x too large\n",
                    ph.p_vaddr);
            continue;
        }

        if (fseek(f, ph.p_offset, SEEK_SET) != 0) break;
        size_t got = fread(ps2_ram + vaddr, 1, ph.p_filesz, f);
        if (ph.p_memsz > ph.p_filesz)
            memset(ps2_ram + vaddr + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);

        fprintf(stderr, "[HOST]   PT_LOAD  vaddr=0x%08x  file=%u  mem=%u  read=%zu\n",
                ph.p_vaddr, ph.p_filesz, ph.p_memsz, got);
    }
    fclose(f);
    return true;
}

// -----------------------------------------------------------------------
// SDL + GL globals — accessed by the frame callback closure
// -----------------------------------------------------------------------
static SDL_Window*   g_win    = nullptr;
static SDL_GLContext g_gl_ctx = nullptr;
static bool          g_running = true;

// Frame callback — called from gs_stub when GS_REG_FINISH is written.
// Runs on the same thread as the game (main thread), so no locking needed.
static void on_frame_done(void) {
    SDL_GL_SwapWindow(g_win);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            g_running = false;
        } else if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE ||
                ev.key.keysym.sym == SDLK_F4) {
                g_running = false;
            }
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = ev.window.data1, h = ev.window.data2;
            gs_set_viewport(w, h);
        }
    }

    if (!g_running) {
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        fprintf(stderr, "[HOST] Bye!\n");
        exit(0);
    }
}

// -----------------------------------------------------------------------
// Headless mode — no SDL/GL, just game code execution with signal traps
// -----------------------------------------------------------------------
static std::atomic<uint64_t> g_frame_count{0};
static std::atomic<uint64_t> g_gs_writes{0};
static int g_headless_max_frames = 300;   // stop after N GS_FINISH events

static void headless_frame_cb(void) {
    uint64_t f = ++g_frame_count;
    if (f % 60 == 0)
        fprintf(stderr, "[HEADLESS] frame %-6llu  gs_writes=%-8llu\n",
                (unsigned long long)f, (unsigned long long)g_gs_writes.load());
    if ((int)f >= g_headless_max_frames) {
        fprintf(stderr, "[HEADLESS] Reached %d frames — stopping cleanly.\n",
                g_headless_max_frames);
        exit(0);
    }
}

// Signal handler — print which signal killed us and where (approximate).
// We use a global volatile to record the last PC the game was tracking.
volatile uint32_t g_last_pc = 0;

static void signal_handler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV (segfault)";        break;
        case SIGBUS:  name = "SIGBUS  (bus error)";        break;
        case SIGFPE:  name = "SIGFPE  (FP exception)";    break;
        case SIGILL:  name = "SIGILL  (illegal opcode)";  break;
        case SIGABRT: name = "SIGABRT (abort)";           break;
    }
    fprintf(stderr,
        "\n[CRASH] Signal %d — %s\n"
        "[CRASH] frames completed : %llu\n"
        "[CRASH] GS register writes: %llu\n"
        "[CRASH] last tracked PC  : 0x%08x\n",
        sig, name,
        (unsigned long long)g_frame_count.load(),
        (unsigned long long)g_gs_writes.load(),
        (unsigned int)g_last_pc);
    // Re-raise with default handler so core dump is produced
    signal(sig, SIG_DFL);
    raise(sig);
}

extern "C" void dump_syscall_stats(void);

static void sigalrm_handler(int) {
    fprintf(stderr, "[HEADLESS] SIGALRM — dumping stats after timeout:\n");
    dump_syscall_stats();
    fprintf(stderr, "[HEADLESS] frames=%llu  gs_writes=%llu\n",
            (unsigned long long)g_frame_count.load(),
            (unsigned long long)g_gs_writes.load());
    signal(SIGALRM, SIG_DFL);
    raise(SIGALRM);
}

static void install_signal_handlers(void) {
    signal(SIGSEGV, signal_handler);
    signal(SIGBUS,  signal_handler);
    signal(SIGFPE,  signal_handler);
    signal(SIGILL,  signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGALRM, sigalrm_handler);
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(int argc, char** argv) {
    fprintf(stderr,
        "=======================================================\n"
        " PS2 Static Recompiler — Host Runtime\n"
        " God of War (USA)  —  SCUS_973.99\n"
        "=======================================================\n");

    // ------------------------------------------------------------------
    // Argument parsing
    //   --headless [--frames N]   Run without SDL/GL; game code only
    //   <elf-path>                Load ELF segments into ps2_ram
    // ------------------------------------------------------------------
    const char* elf_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_headless_max_frames = atoi(argv[++i]);
        } else {
            elf_path = argv[i];
        }
    }

    // ------------------------------------------------------------------
    // Load ELF into ps2_ram (always, whether headless or not)
    // ------------------------------------------------------------------
    if (elf_path) {
        fprintf(stderr, "[HOST] Loading ELF: %s\n", elf_path);
        load_elf(elf_path);
    } else {
        fprintf(stderr,
            "[HOST] No ELF provided. Usage:\n"
            "       %s [--headless [--frames N]] path/to/SCUS_973.99.elf\n"
            "[HOST] Continuing with zeroed ps2_ram\n", argv[0]);
    }

    // ------------------------------------------------------------------
    // Headless path — skip all SDL/GL, install signal handlers, run game
    // ------------------------------------------------------------------
    if (g_headless) {
        fprintf(stderr,
            "[HEADLESS] Mode enabled — no SDL/GL\n"
            "[HEADLESS] Will stop after %d GS_FINISH events\n",
            g_headless_max_frames);
        install_signal_handlers();
        alarm(25);   // SIGALRM after 25s → dump syscall stats then exit
        gs_set_frame_callback(headless_frame_cb);
        fprintf(stderr, "[HEADLESS] Calling ps2_game_start()...\n");
        ps2_game_start();
        fprintf(stderr, "[HEADLESS] ps2_game_start() returned (frames=%llu)\n",
                (unsigned long long)g_frame_count.load());
        return 0;
    }

    // ------------------------------------------------------------------
    // Normal path — SDL2 + OpenGL 3.3
    // ------------------------------------------------------------------
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[HOST] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,  8);

    const int W = 640, H = 480;
    g_win = SDL_CreateWindow(
        "God of War — PS2 Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        W, H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        fprintf(stderr, "[HOST] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_win);
    if (!g_gl_ctx) {
        fprintf(stderr, "[HOST] SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win); SDL_Quit(); return 1;
    }
    SDL_GL_SetSwapInterval(1);

    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK) {
        fprintf(stderr, "[HOST] glewInit: %s\n", glewGetErrorString(glew_err));
        return 1;
    }

    gs_gl_init(W, H);
    gs_set_frame_callback(on_frame_done);
    gs_frame_begin();

    fprintf(stderr, "[HOST] Starting game loop (ESC or F4 to quit)...\n");
    ps2_game_start();

    on_frame_done();
    return 0;
}
