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
            // TODO: map SDL keys → PS2 DualShock 2 input
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = ev.window.data1, h = ev.window.data2;
            gs_set_viewport(w, h);
        }
    }

    if (!g_running) {
        // Teardown and exit cleanly
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        fprintf(stderr, "[HOST] Bye!\n");
        exit(0);
    }
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
    // Optional: load ELF from disk into ps2_ram so runtime data
    // (globals, VTables, pre-initialised arrays) is available.
    // The recompiled code (output.c) has hardcoded addresses baked in;
    // matching RAM content makes dynamic data references work correctly.
    // ------------------------------------------------------------------
    if (argc >= 2) {
        fprintf(stderr, "[HOST] Loading ELF: %s\n", argv[1]);
        load_elf(argv[1]);
    } else {
        fprintf(stderr,
            "[HOST] No ELF provided. Run as:\n"
            "       %s path/to/SCUS_973.99.elf\n"
            "[HOST] Continuing with zeroed ps2_ram (hardcoded code may still run)\n",
            argv[0]);
    }

    // ------------------------------------------------------------------
    // SDL2 init
    // ------------------------------------------------------------------
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[HOST] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Request OpenGL 3.3 Core
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
    SDL_GL_SetSwapInterval(1);  // VSync on

    // ------------------------------------------------------------------
    // GLEW init (must be after context creation)
    // ------------------------------------------------------------------
    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK) {
        fprintf(stderr, "[HOST] glewInit: %s\n", glewGetErrorString(glew_err));
        return 1;
    }

    // ------------------------------------------------------------------
    // GS stub init + frame callback
    // ------------------------------------------------------------------
    gs_gl_init(W, H);
    gs_set_frame_callback(on_frame_done);
    gs_frame_begin();   // Clear initial frame

    // ------------------------------------------------------------------
    // Run the game — ps2_game_start() is the generated entry that
    // calls func_100008 (ps2_entry) and runs forever.
    // GS_REG_FINISH writes trigger on_frame_done → SDL_GL_SwapWindow.
    // ------------------------------------------------------------------
    fprintf(stderr, "[HOST] Starting game loop (ESC or F4 to quit)...\n");
    ps2_game_start();

    // Should not reach here under normal operation
    on_frame_done();  // Final cleanup via g_running=false path
    return 0;
}
