// gs_stub.cpp — PS2 Graphics Synthesizer → OpenGL 3.3
//
// Translates GS register writes to OpenGL state changes and draw calls.
// The GS is a tile-based rasteriser fed by a state machine:
//   1. Write PRIM    → set primitive type + attributes
//   2. Write RGBAQ   → set current colour
//   3. Write ST/UV   → set current texture coords
//   4. Write XYZ2    → submit vertex (drawing kick on last vertex)
//
// Compile target: g++ -std=c++17 (MX Linux / Debian)
// Dependencies:   SDL2, GLEW, OpenGL 3.3 core

#include "../include/ps2_runtime.h"
#include "../include/ps2_gs_regs.h"

#include <SDL2/SDL.h>
#include <GL/glew.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

// -----------------------------------------------------------------------
// Internals — not exposed in header
// -----------------------------------------------------------------------

// Packed vertex as sent to the GPU
struct GSVertex {
    float x, y, z;      // pixel-space (before NDC transform in shader)
    float r, g, b, a;   // normalised [0,1]
    float s, t;         // texture coords
};

// Per-context GS registers
struct GSContext {
    // TEX0
    uint32_t tbp0 = 0;   // Texture buffer base pointer (in 256-byte blocks)
    uint32_t tbw  = 0;   // Texture buffer width (in 64px blocks)
    uint8_t  psm  = 0;   // Pixel storage format
    uint8_t  tw   = 0;   // log2(texture width)
    uint8_t  th   = 0;   // log2(texture height)
    bool     tcc  = false;
    uint8_t  tfx  = GS_TFX_MODULATE;

    // XYOFFSET (fixed-point 12.4 → stored pre-divided)
    float ofx = 0.0f, ofy = 0.0f;

    // SCISSOR
    uint16_t scax0 = 0, scax1 = 639;
    uint16_t scay0 = 0, scay1 = 479;

    // ALPHA (blend equation: (A-B)*C+D)
    uint8_t a_a = 0, a_b = 1, a_c = 0, a_d = 1;
    uint8_t fix = 0x80;   // Fixed alpha (used when a_c==2)

    // TEST
    bool    zte  = true;
    uint8_t ztst = GS_ZTST_GEQUAL;
    bool    ate  = false;
    uint8_t atst = GS_ATST_ALWAYS;
    uint8_t aref = 0;
    uint8_t afail= 0;
    bool    date = false;
    uint8_t datm = 0;

    // FRAME
    uint32_t fbp  = 0;
    uint32_t fbw  = 0;
    uint8_t  fpsm = 0;
    uint32_t fbmsk= 0;

    // ZBUF
    uint32_t zbp  = 0;
    uint8_t  zpsm = 0;
    bool     zmsk = false;
};

// Full GS state machine
static struct GSState {
    // ---- PRIM register fields ----
    uint8_t prim_type = GS_PRIM_TRIANGLE;
    bool    prim_iip  = false;   // Gouraud shading
    bool    prim_tme  = false;   // Texture mapping
    bool    prim_fge  = false;   // Fog
    bool    prim_abe  = false;   // Alpha blend
    bool    prim_aa1  = false;   // Anti-alias
    bool    prim_fst  = false;   // Fixed STQ (no perspective divide)
    uint8_t prim_ctxt = 0;       // Active context (0 or 1)

    // ---- Current vertex attributes ----
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    float q = 1.f;
    float s = 0.f, t = 0.f;      // texture coords (from ST or UV)
    float fog = 0.f;

    // ---- Vertex assembly ----
    std::vector<GSVertex> vbuf;   // accumulated vertices for current primitive
    GSVertex              prev;   // previous vertex (for strips/fans/sprites)
    bool                  has_prev = false;

    // ---- Two drawing contexts ----
    GSContext ctx[2];

    // ---- TEXA ----
    uint8_t ta0 = 0, ta1 = 0x80;
    bool    aem = false;

    // ---- FOGCOL ----
    float fog_r = 0.f, fog_g = 0.f, fog_b = 0.f;

    // ---- Display dims (updated by host on window resize) ----
    int vp_w = 640, vp_h = 480;
} gs;

// -----------------------------------------------------------------------
// OpenGL resources
// -----------------------------------------------------------------------
static GLuint g_prog  = 0;
static GLuint g_vao   = 0;
static GLuint g_vbo   = 0;

// Frame-done callback set by host_main so gs_stub doesn't link SDL
static void (*g_frame_cb)(void) = nullptr;

void gs_set_frame_callback(void (*cb)(void)) { g_frame_cb = cb; }

// -----------------------------------------------------------------------
// GLSL shaders
// -----------------------------------------------------------------------
static const char* VERT = R"glsl(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
layout(location=2) in vec2 aUV;
out vec4 vColor;
out vec2 vUV;
uniform vec2 uInvRes;          // 1/viewport_width, 1/viewport_height
void main() {
    // PS2 pixel-space → NDC.  Y is flipped (PS2 top=0, OpenGL bottom=0).
    float nx =  aPos.x * uInvRes.x * 2.0 - 1.0;
    float ny = -(aPos.y * uInvRes.y * 2.0 - 1.0);
    // Normalise 32-bit PS2 Z to [0,1].  GS Z is 32-bit unsigned max.
    float nz = aPos.z / 4294967295.0;
    gl_Position = vec4(nx, ny, nz, 1.0);
    vColor = aColor;
    vUV    = aUV;
}
)glsl";

static const char* FRAG = R"glsl(
#version 330 core
in  vec4 vColor;
in  vec2 vUV;
out vec4 FragColor;
uniform bool      uTexEnable;
uniform sampler2D uTex;
uniform int       uTfx;        // texture function
void main() {
    vec4 c = vColor;
    if (uTexEnable) {
        vec4 tex = texture(uTex, vUV);
        if (uTfx == 0)      c = c * tex;                          // MODULATE
        else if (uTfx == 1) c = tex;                              // DECAL
        else if (uTfx == 2) c = vec4(c.rgb * tex.rgb + c.aaa, c.a * tex.a); // HIGHLIGHT
        else                c = vec4(c.rgb * tex.rgb + c.aaa, tex.a);        // HIGHLIGHT2
    }
    FragColor = c;
}
)glsl";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "[GS] Shader compile error:\n%s\n", log);
    }
    return s;
}

// -----------------------------------------------------------------------
// Public: called by host_main after SDL + GLEW init
// -----------------------------------------------------------------------
extern "C" void gs_gl_init(int vp_w, int vp_h) {
    gs.vp_w = vp_w;
    gs.vp_h = vp_h;

    // Build shader program
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VERT);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(g_prog, 512, nullptr, log);
               fprintf(stderr, "[GS] Link error: %s\n", log); }
    glDeleteShader(vs); glDeleteShader(fs);

    // VAO / VBO — stride: 9 floats per vertex (xyz + rgba + st)
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    const GLsizei stride = 9 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.f, 0.f, 0.f, 1.f);

    fprintf(stderr, "[GS] OpenGL %s — %dx%d\n",
            (const char*)glGetString(GL_VERSION), vp_w, vp_h);
}

// Called by host on window resize
extern "C" void gs_set_viewport(int w, int h) {
    gs.vp_w = w; gs.vp_h = h;
    glViewport(0, 0, w, h);
}

// -----------------------------------------------------------------------
// Flush accumulated vertices → one OpenGL draw call
// -----------------------------------------------------------------------
static void gs_flush() {
    if (gs.vbuf.empty()) return;
    GSContext& ctx = gs.ctx[gs.prim_ctxt];

    // Pack into flat float array
    std::vector<float> data;
    data.reserve(gs.vbuf.size() * 9);
    for (auto& v : gs.vbuf) {
        data.push_back(v.x); data.push_back(v.y); data.push_back(v.z);
        data.push_back(v.r); data.push_back(v.g); data.push_back(v.b); data.push_back(v.a);
        data.push_back(v.s); data.push_back(v.t);
    }

    glUseProgram(g_prog);

    // Uniforms
    glUniform2f(glGetUniformLocation(g_prog, "uInvRes"),
                1.f / gs.vp_w, 1.f / gs.vp_h);
    glUniform1i(glGetUniformLocation(g_prog, "uTexEnable"), gs.prim_tme ? 1 : 0);
    glUniform1i(glGetUniformLocation(g_prog, "uTfx"),       ctx.tfx);

    // Alpha blend
    if (gs.prim_abe) {
        glEnable(GL_BLEND);
        // PS2 blend: (A-B)*C+D where A,B=RGB sources, C=alpha, D=dest
        // Most common case maps to standard src-alpha blend
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }

    // Depth test
    if (ctx.zte && !ctx.zmsk) {
        glEnable(GL_DEPTH_TEST);
        static const GLenum ztbl[] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
        glDepthFunc(ztbl[ctx.ztst & 3]);
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    // Scissor (PS2 Y=0 is top, OpenGL Y=0 is bottom)
    glEnable(GL_SCISSOR_TEST);
    int sx = ctx.scax0;
    int sy = gs.vp_h - (int)ctx.scay1 - 1;
    int sw = ctx.scax1 - ctx.scax0 + 1;
    int sh = ctx.scay1 - ctx.scay0 + 1;
    if (sw > 0 && sh > 0) glScissor(sx, sy, sw, sh);

    // Upload & draw
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(data.size() * sizeof(float)),
                 data.data(), GL_STREAM_DRAW);

    static const GLenum prim_map[] = {
        GL_POINTS,          // 0 POINT
        GL_LINES,           // 1 LINE
        GL_LINE_STRIP,      // 2 LINESTRIP
        GL_TRIANGLES,       // 3 TRIANGLE
        GL_TRIANGLE_STRIP,  // 4 TRISTRIP
        GL_TRIANGLE_FAN,    // 5 TRIFAN
        GL_TRIANGLES,       // 6 SPRITE (pre-expanded to 2 tris)
    };
    glDrawArrays(prim_map[gs.prim_type], 0, (GLsizei)gs.vbuf.size());
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);

    gs.vbuf.clear();
}

// -----------------------------------------------------------------------
// Vertex submission — called when XYZ2 / XYZF2 is written
// -----------------------------------------------------------------------
static void gs_push_vertex(float raw_x, float raw_y, float raw_z) {
    GSContext& ctx = gs.ctx[gs.prim_ctxt];

    // GS XY are 12.4 fixed-point (divide by 16) minus the XYOFFSET
    float px = (raw_x / 16.f) - ctx.ofx;
    float py = (raw_y / 16.f) - ctx.ofy;
    float pz = raw_z;

    GSVertex v;
    v.x = px; v.y = py; v.z = pz;
    v.r = gs.r; v.g = gs.g; v.b = gs.b; v.a = gs.a;
    v.s = gs.s; v.t = gs.t;

    if (gs.prim_type == GS_PRIM_SPRITE) {
        // Sprite = axis-aligned rectangle defined by two vertices (TL, BR).
        // Expand to two triangles on the second vertex.
        if (!gs.has_prev) {
            gs.prev     = v;
            gs.has_prev = true;
        } else {
            GSVertex& tl = gs.prev;
            GSVertex& br = v;
            // TL, TR, BL, TR, BR, BL
            GSVertex tr = tl; tr.x = br.x; tr.s = br.s;
            GSVertex bl = tl; bl.y = br.y; bl.t = br.t;
            gs.vbuf.push_back(tl);
            gs.vbuf.push_back(tr);
            gs.vbuf.push_back(bl);
            gs.vbuf.push_back(tr);
            gs.vbuf.push_back(br);
            gs.vbuf.push_back(bl);
            gs.has_prev = false;
            gs_flush();
        }
        return;
    }

    gs.vbuf.push_back(v);

    // Auto-flush on primitive completion
    size_t n = gs.vbuf.size();
    bool flush = false;
    switch (gs.prim_type) {
        case GS_PRIM_POINT:    flush = true;      break;
        case GS_PRIM_LINE:     flush = (n % 2 == 0); break;
        case GS_PRIM_TRIANGLE: flush = (n % 3 == 0); break;
        default: break;  // STRIP/FAN: accumulate, flushed on next PRIM or FINISH
    }
    if (flush) gs_flush();
}

// -----------------------------------------------------------------------
// Frame boundary helpers (called by host_main)
// -----------------------------------------------------------------------
extern "C" void gs_frame_begin(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

extern "C" void gs_frame_end(void) {
    gs_flush();
    if (g_frame_cb) g_frame_cb();   // host does SDL_GL_SwapWindow + PollEvent
}

// -----------------------------------------------------------------------
// gs_write_reg — the main entry point called by generated output.c
// -----------------------------------------------------------------------
extern "C" void gs_write_reg(uint64_t reg, uint64_t val) {
    const uint8_t r = (uint8_t)(reg & 0xFF);
    GSContext& ctx = gs.ctx[gs.prim_ctxt];

    switch (r) {

    // ---- Primitive setup ----
    case GS_REG_PRIM:
        gs_flush();
        gs.has_prev  = false;
        gs.prim_type = (uint8_t)(val & 0x7);
        gs.prim_iip  = (val >> 3) & 1;
        gs.prim_tme  = (val >> 4) & 1;
        gs.prim_fge  = (val >> 5) & 1;
        gs.prim_abe  = (val >> 6) & 1;
        gs.prim_aa1  = (val >> 7) & 1;
        gs.prim_fst  = (val >> 8) & 1;
        gs.prim_ctxt = (val >> 9) & 1;
        break;

    // ---- Colour ----
    case GS_REG_RGBAQ:
        gs.r = (float)( val        & 0xFF) / 255.f;
        gs.g = (float)((val >>  8) & 0xFF) / 255.f;
        gs.b = (float)((val >> 16) & 0xFF) / 255.f;
        // PS2 alpha: 0=transparent, 128=opaque, >128=super-white
        gs.a = std::min(1.f, (float)((val >> 24) & 0xFF) / 128.f);
        { uint32_t qb = (uint32_t)(val >> 32); memcpy(&gs.q, &qb, 4); }
        break;

    // ---- Texture coords (float) ----
    case GS_REG_ST: {
        uint32_t sb = (uint32_t)(val & 0xFFFFFFFF);
        uint32_t tb = (uint32_t)(val >> 32);
        memcpy(&gs.s, &sb, 4);
        memcpy(&gs.t, &tb, 4);
        break;
    }

    // ---- Texture coords (fixed-point 12.4) ----
    case GS_REG_UV:
        gs.s = (float)( val        & 0x3FFF) / 16.f;
        gs.t = (float)((val >> 16) & 0x3FFF) / 16.f;
        break;

    // ---- Vertex (drawing kick) ----
    case GS_REG_XYZ2:
    case GS_REG_XYZF2:
        gs_push_vertex(
            (float)( val        & 0xFFFF),
            (float)((val >> 16) & 0xFFFF),
            (float)((val >> 32) & 0xFFFFFFFFu)
        );
        if (r == GS_REG_XYZF2)
            gs.fog = (float)((val >> 56) & 0xFF);
        break;

    // ---- Vertex (NO drawing kick) ----
    case GS_REG_XYZ3:
    case GS_REG_XYZF3:
        // Store as prev vertex for strips — actual kick on XYZ2/XYZF2
        { GSVertex v;
          v.x = (float)(val & 0xFFFF) / 16.f - ctx.ofx;
          v.y = (float)((val >> 16) & 0xFFFF) / 16.f - ctx.ofy;
          v.z = (float)((val >> 32) & 0xFFFFFFFFu);
          v.r = gs.r; v.g = gs.g; v.b = gs.b; v.a = gs.a;
          v.s = gs.s; v.t = gs.t;
          gs.prev = v; gs.has_prev = true; }
        break;

    // ---- Texture buffer (TEX0) ----
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2: {
        uint8_t ci = (r == GS_REG_TEX0_1) ? 0 : 1;
        GSContext& c = gs.ctx[ci];
        c.tbp0 = (uint32_t)( val        & 0x3FFF);
        c.tbw  = (uint32_t)((val >> 14) & 0x3F);
        c.psm  = (uint8_t) ((val >> 20) & 0x3F);
        c.tw   = (uint8_t) ((val >> 26) & 0xF);
        c.th   = (uint8_t) ((val >> 30) & 0xF);
        c.tcc  =           ((val >> 34) & 1) != 0;
        c.tfx  = (uint8_t) ((val >> 35) & 0x3);
        break;
    }

    // ---- XY offset ----
    case GS_REG_XYOFFSET_1:
        gs.ctx[0].ofx = (float)( val        & 0xFFFF) / 16.f;
        gs.ctx[0].ofy = (float)((val >> 32) & 0xFFFF) / 16.f;
        break;
    case GS_REG_XYOFFSET_2:
        gs.ctx[1].ofx = (float)( val        & 0xFFFF) / 16.f;
        gs.ctx[1].ofy = (float)((val >> 32) & 0xFFFF) / 16.f;
        break;

    // ---- Scissor ----
    case GS_REG_SCISSOR_1:
        gs.ctx[0].scax0 = (uint16_t)( val        & 0x7FF);
        gs.ctx[0].scax1 = (uint16_t)((val >> 16) & 0x7FF);
        gs.ctx[0].scay0 = (uint16_t)((val >> 32) & 0x7FF);
        gs.ctx[0].scay1 = (uint16_t)((val >> 48) & 0x7FF);
        break;
    case GS_REG_SCISSOR_2:
        gs.ctx[1].scax0 = (uint16_t)( val        & 0x7FF);
        gs.ctx[1].scax1 = (uint16_t)((val >> 16) & 0x7FF);
        gs.ctx[1].scay0 = (uint16_t)((val >> 32) & 0x7FF);
        gs.ctx[1].scay1 = (uint16_t)((val >> 48) & 0x7FF);
        break;

    // ---- Alpha blend ----
    case GS_REG_ALPHA_1:
        gs.ctx[0].a_a = (uint8_t)( val        & 0x3);
        gs.ctx[0].a_b = (uint8_t)((val >>  2) & 0x3);
        gs.ctx[0].a_c = (uint8_t)((val >>  4) & 0x3);
        gs.ctx[0].a_d = (uint8_t)((val >>  6) & 0x3);
        gs.ctx[0].fix = (uint8_t)((val >> 32) & 0xFF);
        break;
    case GS_REG_ALPHA_2:
        gs.ctx[1].a_a = (uint8_t)( val        & 0x3);
        gs.ctx[1].a_b = (uint8_t)((val >>  2) & 0x3);
        gs.ctx[1].a_c = (uint8_t)((val >>  4) & 0x3);
        gs.ctx[1].a_d = (uint8_t)((val >>  6) & 0x3);
        gs.ctx[1].fix = (uint8_t)((val >> 32) & 0xFF);
        break;

    // ---- Pixel tests ----
    case GS_REG_TEST_1:
        gs.ctx[0].ate  =           ( val        & 1) != 0;
        gs.ctx[0].atst = (uint8_t)(( val >>  1) & 0x7);
        gs.ctx[0].aref = (uint8_t)(( val >>  4) & 0xFF);
        gs.ctx[0].afail= (uint8_t)(( val >> 12) & 0x3);
        gs.ctx[0].date =           ((val >> 14) & 1) != 0;
        gs.ctx[0].datm = (uint8_t)(( val >> 15) & 1);
        gs.ctx[0].zte  =           ((val >> 16) & 1) != 0;
        gs.ctx[0].ztst = (uint8_t)(( val >> 17) & 0x3);
        break;
    case GS_REG_TEST_2:
        gs.ctx[1].ate  =           ( val        & 1) != 0;
        gs.ctx[1].atst = (uint8_t)(( val >>  1) & 0x7);
        gs.ctx[1].aref = (uint8_t)(( val >>  4) & 0xFF);
        gs.ctx[1].afail= (uint8_t)(( val >> 12) & 0x3);
        gs.ctx[1].date =           ((val >> 14) & 1) != 0;
        gs.ctx[1].datm = (uint8_t)(( val >> 15) & 1);
        gs.ctx[1].zte  =           ((val >> 16) & 1) != 0;
        gs.ctx[1].ztst = (uint8_t)(( val >> 17) & 0x3);
        break;

    // ---- Frame buffer ----
    case GS_REG_FRAME_1:
        gs.ctx[0].fbp   = (uint32_t)( val        & 0x1FF);
        gs.ctx[0].fbw   = (uint32_t)((val >>  8) & 0x3F);
        gs.ctx[0].fpsm  = (uint8_t) ((val >> 24) & 0x3F);
        gs.ctx[0].fbmsk = (uint32_t)( val >> 32);
        break;
    case GS_REG_FRAME_2:
        gs.ctx[1].fbp   = (uint32_t)( val        & 0x1FF);
        gs.ctx[1].fbw   = (uint32_t)((val >>  8) & 0x3F);
        gs.ctx[1].fpsm  = (uint8_t) ((val >> 24) & 0x3F);
        gs.ctx[1].fbmsk = (uint32_t)( val >> 32);
        break;

    // ---- Z buffer ----
    case GS_REG_ZBUF_1:
        gs.ctx[0].zbp  = (uint32_t)(val & 0x1FF);
        gs.ctx[0].zpsm = (uint8_t) ((val >> 24) & 0xF);
        gs.ctx[0].zmsk =            ((val >> 32) & 1) != 0;
        break;
    case GS_REG_ZBUF_2:
        gs.ctx[1].zbp  = (uint32_t)(val & 0x1FF);
        gs.ctx[1].zpsm = (uint8_t) ((val >> 24) & 0xF);
        gs.ctx[1].zmsk =            ((val >> 32) & 1) != 0;
        break;

    // ---- Texture alpha ----
    case GS_REG_TEXA:
        gs.ta0 = (uint8_t)( val        & 0xFF);
        gs.aem =           ((val >> 15) & 1) != 0;
        gs.ta1 = (uint8_t)((val >> 32) & 0xFF);
        break;

    // ---- Fog colour ----
    case GS_REG_FOGCOL:
        gs.fog_r = (float)( val        & 0xFF) / 255.f;
        gs.fog_g = (float)((val >>  8) & 0xFF) / 255.f;
        gs.fog_b = (float)((val >> 16) & 0xFF) / 255.f;
        break;

    // ---- Flush texture cache ----
    case GS_REG_TEXFLUSH:
        break;   // No texture cache in stub — no-op

    // ---- Frame sync ----
    case GS_REG_FINISH:
        gs_frame_end();
        gs_frame_begin();
        break;

    case GS_REG_SIGNAL:
    case GS_REG_LABEL:
        break;   // Interrupt signals — not handled in stub

    // ---- Ignored presentation/effect registers ----
    case GS_REG_DTHE:
    case GS_REG_COLCLAMP:
    case GS_REG_PABE:
    case GS_REG_DIMX:
    case GS_REG_SCANMSK:
    case GS_REG_PRMODECONT:
    case GS_REG_PRMODE:
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
    case GS_REG_HWREG:
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    case GS_REG_TEX1_1: case GS_REG_TEX1_2:
    case GS_REG_TEX2_1: case GS_REG_TEX2_2:
    case GS_REG_TEXCLUT:
        break;

    default:
        // Unknown register — logged only in debug builds
#ifdef PS2_DEBUG
        fprintf(stderr, "[GS] Unknown reg 0x%02x = 0x%016llx\n",
                r, (unsigned long long)val);
#endif
        break;
    }
}
