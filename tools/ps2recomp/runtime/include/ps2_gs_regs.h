#pragma once
/* ps2_gs_regs.h — PS2 Graphics Synthesizer register addresses
 * Source: PS2 GS User's Manual (SCEI)
 * Used by gs_stub.cpp when dispatching gs_write_reg() calls.
 */

/* Primitive / vertex attribute registers */
#define GS_REG_PRIM         0x00  /* Primitive type and attributes       */
#define GS_REG_RGBAQ        0x01  /* RGBA + Q (perspective correct)      */
#define GS_REG_ST           0x02  /* Texture coords (float S, T)         */
#define GS_REG_UV           0x03  /* Texture coords (fixed-point U, V)   */
#define GS_REG_XYZF2        0x04  /* Vertex XYZ + fog (drawing kick)     */
#define GS_REG_XYZ2         0x05  /* Vertex XYZ (drawing kick)           */
#define GS_REG_TEX0_1       0x06  /* Texture buffer/format — ctx 1       */
#define GS_REG_TEX0_2       0x07  /* Texture buffer/format — ctx 2       */
#define GS_REG_CLAMP_1      0x08  /* Texture wrapping mode — ctx 1       */
#define GS_REG_CLAMP_2      0x09  /* Texture wrapping mode — ctx 2       */
#define GS_REG_FOG          0x0A  /* Fog parameter                       */
#define GS_REG_XYZF3        0x0C  /* Vertex XYZ + fog (no drawing kick)  */
#define GS_REG_XYZ3         0x0D  /* Vertex XYZ (no drawing kick)        */

/* Texture parameters */
#define GS_REG_TEX1_1       0x14  /* Texture filtering — ctx 1           */
#define GS_REG_TEX1_2       0x15  /* Texture filtering — ctx 2           */
#define GS_REG_TEX2_1       0x16  /* Texture attributes — ctx 1          */
#define GS_REG_TEX2_2       0x17  /* Texture attributes — ctx 2          */

/* Pixel drawing setup */
#define GS_REG_XYOFFSET_1   0x18  /* XY drawing offset — ctx 1           */
#define GS_REG_XYOFFSET_2   0x19  /* XY drawing offset — ctx 2           */
#define GS_REG_PRMODECONT   0x1A  /* Primitive mode control              */
#define GS_REG_PRMODE       0x1B  /* Primitive mode (like PRIM w/o type) */
#define GS_REG_TEXCLUT      0x1C  /* CLUT position in transfer buffer    */
#define GS_REG_SCANMSK      0x22  /* Raster scan mask                    */
#define GS_REG_MIPTBP1_1    0x34  /* MIP table 1 — ctx 1                 */
#define GS_REG_MIPTBP1_2    0x35  /* MIP table 1 — ctx 2                 */
#define GS_REG_MIPTBP2_1    0x36  /* MIP table 2 — ctx 1                 */
#define GS_REG_MIPTBP2_2    0x37  /* MIP table 2 — ctx 2                 */
#define GS_REG_TEXA         0x3B  /* Texture alpha control               */
#define GS_REG_FOGCOL       0x3D  /* Fog color                           */
#define GS_REG_TEXFLUSH     0x3F  /* Flush texture cache                 */

/* Pixel operation setup */
#define GS_REG_SCISSOR_1    0x40  /* Scissor rect — ctx 1                */
#define GS_REG_SCISSOR_2    0x41  /* Scissor rect — ctx 2                */
#define GS_REG_ALPHA_1      0x42  /* Alpha blend equation — ctx 1        */
#define GS_REG_ALPHA_2      0x43  /* Alpha blend equation — ctx 2        */
#define GS_REG_DIMX         0x44  /* Dithering matrix                    */
#define GS_REG_DTHE         0x45  /* Dithering enable                    */
#define GS_REG_COLCLAMP     0x46  /* Color clamp control                 */
#define GS_REG_TEST_1       0x47  /* Pixel test (alpha/depth/etc) ctx 1  */
#define GS_REG_TEST_2       0x48  /* Pixel test — ctx 2                  */
#define GS_REG_PABE         0x49  /* Alpha-blend pixel enable            */
#define GS_REG_FBA_1        0x4A  /* Frame buffer alpha — ctx 1          */
#define GS_REG_FBA_2        0x4B  /* Frame buffer alpha — ctx 2          */

/* Frame/depth buffer */
#define GS_REG_FRAME_1      0x4C  /* Frame buffer setting — ctx 1        */
#define GS_REG_FRAME_2      0x4D  /* Frame buffer setting — ctx 2        */
#define GS_REG_ZBUF_1       0x4E  /* Z buffer setting — ctx 1            */
#define GS_REG_ZBUF_2       0x4F  /* Z buffer setting — ctx 2            */

/* Texel transfer (VRAM DMA) */
#define GS_REG_BITBLTBUF    0x50  /* Texture buffer size / format        */
#define GS_REG_TRXPOS       0x51  /* Texel transfer position             */
#define GS_REG_TRXREG       0x52  /* Texel transfer size                 */
#define GS_REG_TRXDIR       0x53  /* Texel transfer direction            */
#define GS_REG_HWREG        0x54  /* VRAM data transfer port             */

/* Interrupt / sync */
#define GS_REG_SIGNAL       0x60  /* Generate SIGNAL interrupt           */
#define GS_REG_FINISH       0x61  /* Generate FINISH interrupt           */
#define GS_REG_LABEL        0x62  /* Generate LABEL interrupt            */

/* GS PRIM types */
#define GS_PRIM_POINT       0
#define GS_PRIM_LINE        1
#define GS_PRIM_LINESTRIP   2
#define GS_PRIM_TRIANGLE    3
#define GS_PRIM_TRISTRIP    4
#define GS_PRIM_TRIFAN      5
#define GS_PRIM_SPRITE      6

/* Z test methods (TEST register ztst field) */
#define GS_ZTST_NEVER       0
#define GS_ZTST_ALWAYS      1
#define GS_ZTST_GEQUAL      2
#define GS_ZTST_GREATER     3

/* Alpha test methods (TEST register atst field) */
#define GS_ATST_NEVER       0
#define GS_ATST_ALWAYS      1
#define GS_ATST_LESS        2
#define GS_ATST_LEQUAL      3
#define GS_ATST_EQUAL       4
#define GS_ATST_GEQUAL      5
#define GS_ATST_GREATER     6
#define GS_ATST_NOTEQUAL    7

/* Texture function (TEX0 tfx field) */
#define GS_TFX_MODULATE     0
#define GS_TFX_DECAL        1
#define GS_TFX_HIGHLIGHT    2
#define GS_TFX_HIGHLIGHT2   3
