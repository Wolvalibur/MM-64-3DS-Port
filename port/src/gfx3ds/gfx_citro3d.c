#ifdef TARGET_N3DS

#include "gfx_3ds.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#define u64 __u64
#define s64 __s64
#define u32 __u32
#define vu32 __vu32
#define vs32 __vs32
#define s32 __s32
#define u16 __u16
#define s16 __s16
#define u8 __u8
#define s8 __s8
#include <3ds/types.h>
#undef u64 
#undef s64 
#undef u32 
#undef vu32 
#undef vs32 
#undef s32 
#undef u16 
#undef s16 
#undef u8
#undef s8

#include <PR/gbi.h>

#include <3ds.h>
#include <citro3d.h>
#include "c3d_fast.h"
extern void Port3ds_CacheFlush(const void* p, u32 size); /* gfx_3ds.c */

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

static DVLB_s* sVShaderDvlb;
static void* sVboBuffer;

extern const u8 shader_shbin[];
/* objcopy's _size symbol is ABSOLUTE (its address is the size): reading it as a u32 reads address
 * 0xB0, which Azahar allows but real hardware faults on. Use end - start. */
extern const u8 shader_shbin_end[];

/* PORT (2026-09-25): N64 color combiner -> PICA TEV compiler.
 *
 * gfx_pc.c hands over the complete combiner (gfx_cc.h: both cycles, both channels, every input of
 * (A-B)*C+D). Each cycle/channel is lowered to 1..3 TEV operations (lowerChannel), RGB and alpha run
 * side by side in the same stages, cycle 1 follows cycle 0, and fog (blender cycle 1: lerp to the
 * fog color by shade alpha) is a last stage. Inputs:
 *   TEXEL0/1 -> GPU_TEXTURE0/1, SHADE -> GPU_PRIMARY_COLOR (the vertex color is always shade),
 *   PRIM, ENV, their alpha broadcasts, LOD fractions, ONE, ZERO, fog color -> per-stage CONSTANT
 *   (each stage has its own RGB and alpha constant, filled per draw from GfxCombineConsts),
 *   a second constant in one stage -> the TEV buffer's initial color (GPU_PREVIOUS_BUFFER),
 *   COMBINED -> GPU_PREVIOUS in cycle 1's first stage, else the TEV buffer (written by the last
 *   cycle-0 stage; PICA: stage s reads the buffer as updated by stages <= s-2).
 * Keys this can't express exactly are compiled approximately and reported once (PortDbgX). */

enum { KC_NONE, KC_ZERO, KC_ONE, KC_PRIM, KC_PRIMA, KC_ENV, KC_ENVA, KC_LODF, KC_PRIMLODF, KC_FOG, KC_HALF };
enum { OK_CONST, OK_TEX0, OK_TEX1, OK_SHADE, OK_PREV, OK_COMB };

typedef struct {
    u8 kind, alpha, inv, k; /* alpha: use the source's alpha; inv: 1 - x; k: KC_* for OK_CONST */
} Opnd;

typedef struct {
    u8 func; /* GPU_COMBINEFUNC */
    Opnd src[3];
    u8 scale; /* 1: the stage's result times 2 (then clamped) */
} ChanOp;

typedef struct {
    u8 func[2];     /* [channel] GPU_COMBINEFUNC */
    u8 src[2][3];   /* GPU_TEVSRC */
    u8 op[2][3];    /* GPU_TEVOP_RGB / GPU_TEVOP_A */
    u8 konst[2];    /* KC_* in this stage's CONSTANT, per channel */
    u8 scale[2];    /* [channel] 1 = GPU_TEVSCALE_2 */
} TevStage;

#define TEV_STAGES 6

struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    u8 num_stages;
    TevStage stages[TEV_STAGES];
    u8 buf_konst[2];  /* KC_* held in the TEV buffer's initial color, per channel */
    u8 buf_update[2]; /* TEV buffer update masks (stages 0..3), per channel */
};

/* OoT uses many distinct combiners (the sm64 [64] pool overflowed); lookups only happen when the key changes */
#define SHADER_POOL_CAP 1024
static struct ShaderProgram sShaderProgramPool[SHADER_POOL_CAP];
static uint16_t sShaderProgramPoolSize;

static int sCurShader = 0;
static struct GfxCombineConsts sConsts;

static C3D_Tex sTexturePool[4096];
/* PORT (2026-10-01): in the GPU vertex path texture unit 0 samples in projective mode (s/q, t/q per pixel)
 * for the screen-linear shading mode (shader_gpu.v.pica); the shader sends q = 1 otherwise, which samples
 * exactly like 2D. Only unit 0's binding gets the projective type, through a copy of the texture: the pool
 * keeps 2D (a pool texture with the projective type bound to unit 1 - the two-texture sky blend - sampled
 * wrong in Azahar: grey title-screen sky, tools/statediff bootflow.py title). */
extern int gPortGpuVtx, gPortShadeLinear;
static C3D_Tex sTex0Proj;
static void texBindUnit(int unit, C3D_Tex* t) {
    if (unit == 0 && gPortGpuVtx && gPortShadeLinear) {
        sTex0Proj = *t;
        sTex0Proj.param = (t->param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
        C3D_TexBind(0, &sTex0Proj);
    } else {
        C3D_TexBind(unit, t);
    }
}
static float sTexturePoolScaleS[4096];
static float sTexturePoolScaleT[4096];
static int sTextureIndex;
static int sCurTex = 0;

static int sTexUnits[2];

static bool gfx_citro3d_z_is_from_0_to_1(void)
{
    return true;
}

#define VTX_FLOATS 13 /* gfx_pc.c packs pos(4) uv0(2) uv1(2) shade(4) stereo(1); the VBO holds the same layout */

static bool sDepthTestOn = false;
static bool sDepthUpdateOn = true;
static bool sDepthDecal = false;
static bool sDepthXlu = false; /* ZMODE_XLU (gfx_pc.c): the RDP passes it only when strictly nearer */
static int sDepthRoom = 0; /* drawing room geometry (z_room.c tags, gfx_pc.c sInRoomDl): 0 no, else the 1-based mesh entry */
/* PORT (2026-10-28): linear depth (W-buffer) for perspective draws: 0 = the z/w buffer, else the projection's near
 * distance as -P[3][2] / 2 (gfx_pc.c sets it per draw). See updateDepth. */
static float sDepthLinN = 0.0f;
#define DEPTH_W_MAX 32768.0f /* linear depth = w / DEPTH_W_MAX (farther than any MM far plane) */

static bool sUseBlend;

static int sBufIdx = 0;

/* PORT (2026-09-30): 60 fps replay (gfx_pc.c "replay"). While recording, every state call and draw is
 * logged instead of drawn (state calls still run: the texture/shader bookkeeping and citro3d's state
 * are needed by the walk); gfx_citro3d_replay re-issues the log for each shown frame, after gfx_pc.c
 * rewrote the VBO positions. Texture uploads are not logged - the textures stay in their slots. */
enum { OP_SHADER, OP_CONSTS, OP_TEX, OP_SAMPLER, OP_DTEST, OP_DMASK, OP_DECAL, OP_VIEWPORT, OP_SCISSOR,
       OP_ALPHA, OP_DRAWID, OP_STEREO, OP_DRAWIDX, OP_GPUPAL, OP_GPUPARAM, OP_CULL, OP_RAWMODE, OP_RAWPARAM, OP_TEXPARAM };
typedef struct {
    u8 op;
    int v[4];
} RecOp;
#define REC_OPS 12288
#define REC_CONSTS 4096
static RecOp* sOps;
static struct GfxCombineConsts* sOpConsts;
static int sOpN, sConstN, sRec, sRecOverflow;
static int sRecDirect; /* the walk draws the update's first shown frame itself (replay by copy: captured as it goes) */
static void recOp(u8 op, int a, int b, int c, int d) {
    if (sOpN >= REC_OPS) {
        sRecOverflow = 1;
        return;
    }
    sOps[sOpN].op = op;
    sOps[sOpN].v[0] = a, sOps[sOpN].v[1] = b, sOps[sOpN].v[2] = c, sOps[sOpN].v[3] = d;
    sOpN++;
}

static void gfx_citro3d_unload_shader(struct ShaderProgram *old_prg) {

}

static Opnd opnd(u8 kind, u8 alpha, u8 k) {
    Opnd o = { kind, alpha, 0, k };
    return o;
}

static bool opndEq(Opnd a, Opnd b) {
    return a.kind == b.kind && a.alpha == b.alpha && a.inv == b.inv && a.k == b.k;
}

static bool isK(Opnd a, u8 k) {
    return a.kind == OK_CONST && !a.inv && a.k == k;
}

/* gfx_cc.h source -> operand, for channel ch (0 RGB, 1 alpha: every source is its alpha) */
static Opnd ccsOpnd(u8 s, int ch) {
    switch (s) {
        case CCS_1: return opnd(OK_CONST, ch, KC_ONE);
        case CCS_TEX0: return opnd(OK_TEX0, ch, 0);
        case CCS_TEX1: return opnd(OK_TEX1, ch, 0);
        case CCS_TEX0A: return opnd(OK_TEX0, 1, 0);
        case CCS_TEX1A: return opnd(OK_TEX1, 1, 0);
        case CCS_SHADE: return opnd(OK_SHADE, ch, 0);
        case CCS_SHADEA: return opnd(OK_SHADE, 1, 0);
        case CCS_PRIM: return opnd(OK_CONST, ch, KC_PRIM);
        case CCS_PRIMA: return opnd(OK_CONST, ch, ch ? KC_PRIM : KC_PRIMA);
        case CCS_ENV: return opnd(OK_CONST, ch, KC_ENV);
        case CCS_ENVA: return opnd(OK_CONST, ch, ch ? KC_ENV : KC_ENVA);
        case CCS_LODF: return opnd(OK_CONST, ch, KC_LODF);
        case CCS_PRIMLODF: return opnd(OK_CONST, ch, KC_PRIMLODF);
        case CCS_COMB: return opnd(OK_COMB, ch, 0);
        case CCS_COMBA: return opnd(OK_COMB, 1, 0);
    }
    return opnd(OK_CONST, ch, KC_ZERO);
}

static int emit(ChanOp* out, int n, u8 func, Opnd a, Opnd b, Opnd c) {
    out[n].func = func;
    out[n].scale = 0;
    out[n].src[0] = a;
    out[n].src[1] = b;
    out[n].src[2] = c;
    return n + 1;
}

/* (A-B)*C+D as TEV operations (PICA clamps every stage to 0..1). Returns the count (<= 3);
 * 0 = the channel passes the previous cycle through unchanged. */
static int lowerChannel(Opnd a, Opnd b, Opnd c, Opnd d, int ch, int cycle, ChanOp* out) {
    Opnd z = opnd(OK_CONST, ch, KC_ZERO), prev = opnd(OK_PREV, ch, 0);
    int n = 0;
    if (isK(c, KC_ZERO) || opndEq(a, b)) {
        if (cycle == 1 && d.kind == OK_COMB && d.alpha == ch) {
            return 0;
        }
        return emit(out, n, GPU_REPLACE, d, z, z);
    }
    if (isK(b, KC_ZERO)) {
        if (isK(d, KC_ZERO)) {
            if (isK(a, KC_ONE)) return emit(out, n, GPU_REPLACE, c, z, z);
            if (isK(c, KC_ONE)) return emit(out, n, GPU_REPLACE, a, z, z);
            return emit(out, n, GPU_MODULATE, a, c, z);
        }
        if (isK(a, KC_ONE)) return emit(out, n, GPU_ADD, c, d, z);
        if (isK(c, KC_ONE)) return emit(out, n, GPU_ADD, a, d, z);
        return emit(out, n, GPU_MULTIPLY_ADD, a, c, d);
    }
    if (opndEq(b, d)) {
        return emit(out, n, GPU_INTERPOLATE, a, b, c); /* a*c + b*(1-c) */
    }
    if (isK(a, KC_ONE) && b.kind != OK_PREV) { /* (1-B)*C+D */
        Opnd nb = b;
        nb.inv = 1;
        if (isK(d, KC_ZERO)) return emit(out, n, GPU_MODULATE, nb, c, z);
        if (isK(c, KC_ONE)) return emit(out, n, GPU_ADD, nb, d, z);
        return emit(out, n, GPU_MULTIPLY_ADD, nb, c, d);
    }
    if (isK(b, KC_ONE) && a.kind != OK_PREV) { /* (A-1)*C+D = D - (1-A)*C: no intermediate above 1 */
        Opnd na = a;
        na.inv = 1;
        n = emit(out, n, GPU_MODULATE, na, c, z);
        return emit(out, n, GPU_SUBTRACT, d, prev, z);
    }
    if (isK(d, KC_ZERO)) { /* max(A-B, 0)*C is exact: C >= 0 and the N64 clamps the result */
        n = emit(out, n, GPU_SUBTRACT, a, b, z);
        if (isK(c, KC_ONE)) return n;
        return emit(out, n, GPU_MODULATE, prev, c, z);
    }
    if (isK(c, KC_ONE)) {
        n = emit(out, n, GPU_ADD, a, d, z);
        return emit(out, n, GPU_SUBTRACT, prev, b, z);
    }
    if (opndEq(a, d) && a.kind != OK_PREV && b.kind != OK_PREV && c.kind != OK_PREV) {
        /* PORT (2026-10-05): (A-B)*C + A = 2A - (B*C + A*(1-C)) = 2 * (A - lerp(A, B, C) / 2): every intermediate stays in
         * 0..1 and the only clamp is the last stage's - the N64's own. The order below computed A*C + max(A - B*C, 0),
         * too bright wherever A < B*C: the blue warp's crystal, (TEXEL0 - PRIM) * PRIM_LOD_FRAC + TEXEL0 with the
         * fraction at 1 (= 2T - P on the N64), came out white instead of blue and hid Link (Chamber of the Sages,
         * tools/statediff fbdiff: 10% of the pixels off, user report). */
        n = emit(out, n, GPU_INTERPOLATE, b, a, c);                      /* B*C + A*(1-C) */
        n = emit(out, n, GPU_MODULATE, prev, opnd(OK_CONST, ch, KC_HALF), z); /* ... / 2 */
        n = emit(out, n, GPU_SUBTRACT, a, prev, z);                      /* A - ..., times 2 */
        out[n - 1].scale = 1;
        return n;
    }
    if (a.kind != OK_PREV && b.kind != OK_PREV && c.kind != OK_PREV && d.kind != OK_PREV) {
        /* PORT (2026-09-30): A*C + (D - B*C). Every PICA stage clamps to 0..1; the previous order
         * interp(A,B,C) + D - B saturated at the "+ D" whenever the true intermediate passed 1, then the
         * "- B" made it far too dark: the N64 boot logo's "(TEXEL1 - PRIM) * ENV_A + TEXEL0" text came out
         * dark blue instead of cyan-white (tools/statediff fbdiff, boot@60). This order errs only when
         * D < B*C (a bias larger than the base), which "base texture + shine/tint" combines avoid. */
        n = emit(out, n, GPU_MODULATE, b, c, z);         /* B*C */
        n = emit(out, n, GPU_SUBTRACT, d, prev, z);      /* D - B*C */
        return emit(out, n, GPU_MULTIPLY_ADD, a, c, prev); /* A*C + ... */
    }
    n = emit(out, n, GPU_INTERPOLATE, a, b, c); /* (A-B)*C + B */
    n = emit(out, n, GPU_ADD, prev, d, z);      /* ... + D */
    return emit(out, n, GPU_SUBTRACT, prev, b, z); /* ... - B */
}

/* compile state */
typedef struct {
    struct ShaderProgram* prg;
    bool exact;
    int cyc0Last;         /* last cycle-0 stage */
    bool prevIsComb[2];   /* per component (0 rgb, 1 alpha): PREVIOUS still holds cycle 0's output */
    bool bufComb[2];      /* buffer component carries cycle 0's output (updated by stage cyc0Last) */
    int bufKLast[2];      /* last stage reading the buffer's initial color */
} TevCompile;

static int arityOf(u8 func) {
    return func == GPU_REPLACE ? 1 : (func == GPU_INTERPOLATE || func == GPU_MULTIPLY_ADD) ? 3 : 2;
}

/* buffer component c as constant k at stage s: its initial color is visible until the cycle-0
 * output lands in it (PICA: stage s reads buffer updates of stages <= s-2). PORT (2026-09-29): and
 * only FROM STAGE 1: stage 0's PREVIOUS_BUFFER reads 0 (the buffer color register feeds the buffer
 * that stage 1 sees; Citra/Azahar model it the same way). Using it at stage 0 turned the constant
 * black, e.g. the hearts' (PRIM - ENV) * TEXEL0 + ENV drew a black outline instead of ENV. */
static bool bufKonstOk(TevCompile* tc, int c, u8 k, int s) {
    struct ShaderProgram* prg = tc->prg;
    if (s == 0) {
        return false;
    }
    if (prg->buf_konst[c] != KC_NONE && prg->buf_konst[c] != k) {
        return false;
    }
    if (tc->bufComb[c] && s > tc->cyc0Last + 1) {
        return false;
    }
    return true;
}

/* place op for channel ch into stage s; returns false (placing nothing) if it needs a second
 * constant with nowhere to put it: *needK then names that constant */
static bool placeOp(TevCompile* tc, TevStage* st, int s, int ch, int cycle, const ChanOp* op, u8* needK) {
    struct ShaderProgram* prg = tc->prg;
    int arity = arityOf(op->func);
    u8 konst = st->konst[ch], bufK = prg->buf_konst[ch];
    u8 src[3], ops[3];
    bool useBufK = false, useBufComb[2] = { false, false };
    for (int i = 0; i < 3; i++) {
        Opnd o = op->src[i];
        bool alpha = ch == 1 || o.alpha;
        if (i >= arity) { /* unused by the function: claim nothing */
            src[i] = GPU_PREVIOUS;
            ops[i] = ch ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            continue;
        }
        if (o.kind == OK_COMB && cycle == 0) { /* cleared by gfx_cc_key; keep safe */
            o.kind = OK_CONST;
            o.k = KC_ZERO;
        }
        switch (o.kind) {
            case OK_TEX0: src[i] = GPU_TEXTURE0; break;
            case OK_TEX1: src[i] = GPU_TEXTURE1; break;
            case OK_SHADE: src[i] = GPU_PRIMARY_COLOR; break;
            case OK_PREV: src[i] = GPU_PREVIOUS; break;
            case OK_COMB:
                if (tc->prevIsComb[alpha ? 1 : 0]) {
                    src[i] = GPU_PREVIOUS;
                } else {
                    src[i] = GPU_PREVIOUS_BUFFER;
                    useBufComb[alpha ? 1 : 0] = true;
                }
                break;
            case OK_CONST:
                alpha = ch == 1; /* constants carry per-channel values (KC_PRIMA etc.) */
                if (konst == KC_NONE || konst == o.k) {
                    konst = o.k;
                    src[i] = GPU_CONSTANT;
                } else if ((bufK == KC_NONE || bufK == o.k) && bufKonstOk(tc, ch, o.k, s)) {
                    bufK = o.k;
                    useBufK = true;
                    src[i] = GPU_PREVIOUS_BUFFER;
                } else {
                    *needK = o.k;
                    return false;
                }
                break;
        }
        if (ch == 0) {
            ops[i] = alpha ? (o.inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA)
                           : (o.inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR);
        } else {
            ops[i] = o.inv ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
        }
    }
    for (int c = 0; c < 2; c++) {
        if (useBufComb[c]) {
            if (tc->cyc0Last < 0 || tc->cyc0Last > 3 || s < tc->cyc0Last + 2 || tc->bufKLast[c] > tc->cyc0Last + 1) {
                tc->exact = false;
            } else {
                tc->bufComb[c] = true;
                prg->buf_update[c] = 1 << tc->cyc0Last;
            }
        }
    }
    if (useBufK) {
        prg->buf_konst[ch] = bufK;
        if (s > tc->bufKLast[ch]) {
            tc->bufKLast[ch] = s;
        }
    }
    st->konst[ch] = konst;
    st->func[ch] = op->func;
    st->scale[ch] = op->scale;
    for (int i = 0; i < 3; i++) {
        st->src[ch][i] = src[i];
        st->op[ch][i] = ops[i];
    }
    return true;
}

static ChanOp passOp(int ch) {
    ChanOp op;
    op.func = GPU_REPLACE;
    op.scale = 0;
    op.src[0] = opnd(OK_PREV, ch, 0);
    op.src[1] = op.src[2] = opnd(OK_CONST, ch, KC_ZERO);
    return op;
}

/* compile a key; returns false if some input had to be approximated */
static bool compileTev(struct ShaderProgram* prg) {
    uint64_t id0 = prg->shader_id0;
    TevCompile tc;
    int ns = 0;
    memset(&tc, 0, sizeof(tc));
    tc.prg = prg;
    tc.exact = true;
    tc.cyc0Last = -1;
    tc.bufKLast[0] = tc.bufKLast[1] = -1;
    memset(prg->stages, 0, sizeof(prg->stages));
    prg->buf_konst[0] = prg->buf_konst[1] = KC_NONE;
    prg->buf_update[0] = prg->buf_update[1] = 0;

    for (int cycle = 0; cycle < 2; cycle++) {
        ChanOp ops[2][4];
        int nops[2], next[2] = { 0, 0 };
        for (int ch = 0; ch < 2; ch++) {
            nops[ch] = lowerChannel(ccsOpnd(CC_SRC(id0, cycle, ch, 0), ch), ccsOpnd(CC_SRC(id0, cycle, ch, 1), ch),
                                    ccsOpnd(CC_SRC(id0, cycle, ch, 2), ch), ccsOpnd(CC_SRC(id0, cycle, ch, 3), ch), ch,
                                    cycle, ops[ch]);
        }
        if (cycle == 1) {
            tc.prevIsComb[0] = tc.prevIsComb[1] = true;
        }
        while (next[0] < nops[0] || next[1] < nops[1]) {
            if (ns >= TEV_STAGES) {
                tc.exact = false;
                goto done;
            }
            TevStage* st = &prg->stages[ns];
            bool changed[2] = { false, false };
            for (int ch = 0; ch < 2; ch++) {
                u8 needK = KC_NONE;
                if (next[ch] < nops[ch]) {
                    ChanOp* op = &ops[ch][next[ch]];
                    if (placeOp(&tc, st, ns, ch, cycle, op, &needK)) {
                        next[ch]++;
                        changed[ch] = true;
                        continue;
                    }
                    /* two constants: preload the second into PREVIOUS with this stage, if the op
                     * doesn't need PREVIOUS itself (COMBINED then comes from the buffer) */
                    bool usesPrev = false;
                    for (int i = 0; i < arityOf(op->func); i++) {
                        usesPrev |= op->src[i].kind == OK_PREV;
                    }
                    ChanOp pre;
                    pre.func = GPU_REPLACE;
                    pre.scale = 0;
                    pre.src[0] = opnd(OK_CONST, ch, needK);
                    pre.src[1] = pre.src[2] = pre.src[0];
                    if (!usesPrev && placeOp(&tc, st, ns, ch, cycle, &pre, &needK)) {
                        for (int i = 0; i < 3; i++) {
                            if (op->src[i].kind == OK_CONST && op->src[i].k == pre.src[0].k) {
                                u8 inv = op->src[i].inv;
                                op->src[i] = opnd(OK_PREV, ch, 0);
                                op->src[i].inv = inv;
                            }
                        }
                        changed[ch] = true;
                        continue;
                    }
                    tc.exact = false; /* approximate: drop the op's second constant */
                    for (int i = 0; i < 3; i++) {
                        if (op->src[i].kind == OK_CONST && op->src[i].k == needK) {
                            op->src[i].k = st->konst[ch];
                        }
                    }
                    if (placeOp(&tc, st, ns, ch, cycle, op, &needK)) {
                        next[ch]++;
                        changed[ch] = true;
                        continue;
                    }
                }
                ChanOp pass = passOp(ch);
                if (ns == 0) { /* nothing to pass through at stage 0 */
                    pass.src[0] = opnd(OK_CONST, ch, ch ? KC_ONE : KC_ZERO);
                }
                placeOp(&tc, st, ns, ch, cycle, &pass, &needK);
            }
            for (int ch = 0; ch < 2; ch++) {
                if (changed[ch]) {
                    tc.prevIsComb[ch] = false;
                }
            }
            ns++;
        }
        if (cycle == 0) {
            tc.cyc0Last = ns - 1;
        }
    }
done:
    if (prg->shader_id1 & SHADER_OPT_FOG) {
        if (ns < TEV_STAGES) {
            TevStage* st = &prg->stages[ns++];
            st->func[0] = GPU_INTERPOLATE; /* fog * f + prev * (1 - f), f = shade alpha */
            st->src[0][0] = GPU_CONSTANT;
            st->src[0][1] = GPU_PREVIOUS;
            st->src[0][2] = GPU_PRIMARY_COLOR;
            st->op[0][0] = GPU_TEVOP_RGB_SRC_COLOR;
            st->op[0][1] = GPU_TEVOP_RGB_SRC_COLOR;
            st->op[0][2] = GPU_TEVOP_RGB_SRC_ALPHA;
            st->konst[0] = KC_FOG;
            st->func[1] = GPU_REPLACE;
            st->src[1][0] = st->src[1][1] = st->src[1][2] = GPU_PREVIOUS;
            st->op[1][0] = st->op[1][1] = st->op[1][2] = GPU_TEVOP_A_SRC_ALPHA;
        } else {
            tc.exact = false;
        }
    }
    prg->num_stages = ns;
    return tc.exact;
}

/* value of constant k for channel ch, as 0..255 */
static u8 konstVal(u8 k, int ch, int comp) {
    const struct GfxCombineConsts* c = &sConsts;
    switch (k) {
        case KC_ONE: return 255;
        case KC_PRIM: return c->prim[ch ? 3 : comp];
        case KC_PRIMA: return c->prim[3];
        case KC_ENV: return c->env[ch ? 3 : comp];
        case KC_ENVA: return c->env[3];
        case KC_LODF: return c->lod_frac;
        case KC_PRIMLODF: return c->prim_lod_frac;
        case KC_FOG: return c->fog[ch ? 3 : comp];
        case KC_HALF: return 128;
    }
    return 0;
}

static u32 konstColor(u8 krgb, u8 ka) { /* PICA constant: 0xAABBGGRR */
    return konstVal(krgb, 0, 0) | (konstVal(krgb, 0, 1) << 8) | (konstVal(krgb, 0, 2) << 16) |
           ((u32)konstVal(ka, 1, 3) << 24);
}

/* program the TEV for the current shader and constants */
u32 gPortC3dCalls[6]; /* per report: shader loads, const sets, tex binds (same tex), tex binds, samplers, uploads */
static void updateShader(void)
{
    gPortC3dCalls[0]++;
    if (sCurShader < 0 || sCurShader >= SHADER_POOL_CAP) {
        sCurShader = 0;
    }
    const struct ShaderProgram* prg = &sShaderProgramPool[sCurShader];
    for (int s = 0; s < TEV_STAGES; s++) {
        C3D_TexEnv* e = C3D_GetTexEnv(s);
        C3D_TexEnvInit(e);
        if (s >= prg->num_stages) {
            continue;
        }
        const TevStage* st = &prg->stages[s];
        C3D_TexEnvSrc(e, C3D_RGB, st->src[0][0], st->src[0][1], st->src[0][2]);
        C3D_TexEnvSrc(e, C3D_Alpha, st->src[1][0], st->src[1][1], st->src[1][2]);
        C3D_TexEnvOpRgb(e, st->op[0][0], st->op[0][1], st->op[0][2]);
        C3D_TexEnvOpAlpha(e, st->op[1][0], st->op[1][1], st->op[1][2]);
        C3D_TexEnvFunc(e, C3D_RGB, st->func[0]);
        C3D_TexEnvFunc(e, C3D_Alpha, st->func[1]);
        if (st->scale[0] | st->scale[1]) {
            C3D_TexEnvScale(e, C3D_RGB, st->scale[0] ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
            C3D_TexEnvScale(e, C3D_Alpha, st->scale[1] ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
        }
        C3D_TexEnvColor(e, konstColor(st->konst[0], st->konst[1]));
    }
    C3D_TexEnvBufUpdate(C3D_RGB, prg->buf_update[0]);
    C3D_TexEnvBufUpdate(C3D_Alpha, prg->buf_update[1]);
    C3D_TexEnvBufColor(konstColor(prg->buf_konst[0], prg->buf_konst[1]));
}

static void gfx_citro3d_load_shader(struct ShaderProgram *new_prg) {
    sCurShader = new_prg - sShaderProgramPool;
    if (sRec) recOp(OP_SHADER, sCurShader, 0, 0, 0);
    updateShader();
}

static void gfx_citro3d_set_combine_consts(const struct GfxCombineConsts *consts) {
    /* only the constant colors change: rewrite them, not the whole TEV program */
    const struct ShaderProgram* prg = &sShaderProgramPool[sCurShader];
    int s;
    gPortC3dCalls[1]++;
    sConsts = *consts;
    if (sRec) {
        if (sConstN < REC_CONSTS) {
            sOpConsts[sConstN] = *consts;
            recOp(OP_CONSTS, sConstN++, 0, 0, 0);
        } else {
            sRecOverflow = 1;
        }
    }
    for (s = 0; s < prg->num_stages; s++) {
        if (prg->stages[s].konst[0] != KC_NONE || prg->stages[s].konst[1] != KC_NONE) {
            C3D_TexEnvColor(C3D_GetTexEnv(s), konstColor(prg->stages[s].konst[0], prg->stages[s].konst[1]));
        }
    }
    if (prg->buf_konst[0] != KC_NONE || prg->buf_konst[1] != KC_NONE) {
        C3D_TexEnvBufColor(konstColor(prg->buf_konst[0], prg->buf_konst[1]));
    }
}

static struct ShaderProgram *gfx_citro3d_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram *prg;
    if (sShaderProgramPoolSize >= SHADER_POOL_CAP) {
        /* pool full: recycle the last slot (wrong-but-safe) */
        prg = &sShaderProgramPool[SHADER_POOL_CAP - 1];
    } else {
        prg = &sShaderProgramPool[sShaderProgramPoolSize++];
    }
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->used_textures[0] = (shader_id1 & SHADER_OPT_TEX0) != 0;
    prg->used_textures[1] = (shader_id1 & SHADER_OPT_TEX1) != 0;
    if (!compileTev(prg)) {
        static int sReported;
        if (sReported < 16) {
            extern void PortDbgX(const char* label, unsigned val);
            sReported++;
            PortDbgX("TEV approx key hi", (unsigned)(shader_id0 >> 32));
            PortDbgX("TEV approx key lo", (unsigned)shader_id0);
            PortDbgX("TEV approx opts", (unsigned)shader_id1);
        }
    }
    gfx_citro3d_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_citro3d_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    for (size_t i = 0; i < sShaderProgramPoolSize; i++) {
        if (sShaderProgramPool[i].shader_id0 == shader_id0 && sShaderProgramPool[i].shader_id1 == shader_id1) {
            return &sShaderProgramPool[i];
        }
    }
    return NULL;
}

static void gfx_citro3d_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = 1;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static u32 gfx_citro3d_new_texture(void) {
    if(sTextureIndex == 4096)
    {
        printf("Out of texures!");
        return 0;
    }
    return sTextureIndex++;
}

/* PORT PERF (2026-10-01): a unit already bound to this texture is not re-bound - C3D_TexBind marks the
 * unit dirty and citro3d re-sends its texture registers with the next draw (~32 of ~168 binds per frame
 * were the same texture again). An upload into a bound texture or a sampler change re-binds explicitly. */
static bool sTexBoundOk[2];

static void gfx_citro3d_select_texture(int tile, u32 texture_id) {
    gPortC3dCalls[sTexUnits[tile] == (int)texture_id ? 2 : 3]++;
    if (sRec) recOp(OP_TEX, tile, (int)texture_id, 0, 0);
    if (!(sTexBoundOk[tile] && sTexUnits[tile] == (int)texture_id)) {
        texBindUnit(tile, &sTexturePool[texture_id]);
        sTexBoundOk[tile] = true;
    }
    sCurTex = texture_id;
    sTexUnits[tile] = texture_id;
}

static u32 sTexBuf[16 * 1024] __attribute__((aligned(32)));

static int sTileOrder[] =
{
    0,  1,   4,  5,
    2,  3,   6,  7,

    8,  9,  12, 13,  
    10, 11,  14, 15
};

static void performTexSwizzle(const u8* src, u32* dst, u32 w, u32 h)
{
    int offs = 0;
    for(int y = 0; y < h; y += 8)
    {
        for(int x = 0; x < w; x += 8)
        {
            for (int i = 0; i < 64; i++)
            {
                int x2 = i & 7;
                int y2 = i >> 3;
                int pos = sTileOrder[(x2 & 3) + ((y2 & 3) << 2)] + ((x2 >> 2) << 4) + ((y2 >> 2) << 5);
                u32 c = ((const u32*)src)[(y + y2) * w + x + x2];
                dst[offs + pos] = ((c & 0xFF) << 24) | (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c >> 24);
            }
            dst += 64;
        }
    }
}

int gPortTexPadClamp; /* gfx_pc.c: 1 = S clamped, 2 = T clamped (the texture being uploaded) */
static void gfx_citro3d_upload_texture(uint8_t *rgba32_buf, int width, int height) {
    gPortC3dCalls[5]++;
    if(width < 8 || height < 8 || (width & (width - 1)) || (height & (height - 1)))
    {
        int newWidth = width < 8 ? 8 : (1 << (32 - __builtin_clz(width - 1)));
        int newHeight = height < 8 ? 8 : (1 << (32 - __builtin_clz(height - 1)));
        if(newWidth * newHeight * 4 > sizeof(sTexBuf))
        {
            printf("Tex buffer overflow!\n");
            return;
        }
        int offs = 0;
        for(int y = 0; y < newHeight; y += 8)
        {
            for(int x = 0; x < newWidth; x += 8)
            {
                for (int i = 0; i < 64; i++)
                {
                    int x2 = i % 8;
                    int y2 = i / 8;

                    /* PORT (2026-10-26): padding texels. Was "subtract the size once": for a texture under half
                     * the padded size (64x1 scene title card gradient, 1-3 texel strips) rows/columns past the
                     * second copy read past the image - stale buffer bytes, the streaked purple title banner. A
                     * wrapped axis repeats the texture; a clamped one repeats its edge, so sampling past the
                     * image (UVs are scaled to it, the GPU clamps at the padded edge) and bilinear filtering at
                     * its border see the edge texel, not the other side (seams in the pause pages' pieces). */
                    int realX = x + x2;
                    if(realX >= width)
                        realX = (gPortTexPadClamp & 1) ? width - 1 : realX % width;

                    int realY = y + y2;
                    if(realY >= height)
                        realY = (gPortTexPadClamp & 2) ? height - 1 : realY % height;

                    int pos = sTileOrder[x2 % 4 + y2 % 4 * 4] + 16 * (x2 / 4) + 32 * (y2 / 4);
                    u32 c = ((u32*)rgba32_buf)[realY * width + realX];
                    ((u32*)sTexBuf)[offs + pos] = ((c & 0xFF) << 24) | (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c >> 24);
                }
                offs += 64;
            }
        }
        sTexturePoolScaleS[sCurTex] = width / (float)newWidth;
        sTexturePoolScaleT[sCurTex] = height / (float)newHeight;
        width = newWidth;
        height = newHeight;
    }
    else
    {
        /* PORT (MM bring-up): the same bound as the branch above. Unchecked, a power-of-two texture over 16K texels
         * (MM: larger images than OoT's) overran sTexBuf and corrupted the statics after it - texture records the
         * GPU then read garbage addresses from (Azahar: "Read from unknown GPU address"). */
        if (width * height * 4 > (int)sizeof(sTexBuf)) {
            static int sLogged;
            if (sLogged++ < 4) {
                extern void PortDbgX(const char* label, unsigned val);
                PortDbgX("[gfx] texture too large for sTexBuf: w", width);
                PortDbgX("  h", height);
            }
            return;
        }
        sTexturePoolScaleS[sCurTex] = 1.f;
        sTexturePoolScaleT[sCurTex] = 1.f;
        performTexSwizzle(rgba32_buf, sTexBuf, width, height);
    }
    /* Cache slots get recycled (gfx_pc pool wraps at 512); C3D_TexInit on an
     * already-initialized tex leaks its previous linear buffer. Free it first,
     * or reuse the buffer when the dimensions match. */
    if (sTexturePool[sCurTex].data != NULL &&
        (sTexturePool[sCurTex].width != width || sTexturePool[sCurTex].height != height)) {
        C3D_TexDelete(&sTexturePool[sCurTex]);
        sTexturePool[sCurTex].data = NULL;
    }
    if (sTexturePool[sCurTex].data == NULL) {
        C3D_TexInit(&sTexturePool[sCurTex], width, height, GPU_RGBA8);
    }
#ifdef PORT_TEX_DEBUG /* (debug: textures that are large, failed, or end past the linear heap) */
    {
        extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
        uintptr_t d = (uintptr_t)sTexturePool[sCurTex].data;
        uintptr_t end = d + (uintptr_t)width * height * 4;
        if (d == 0 || width * height > 256 * 256 || end > __ctru_linear_heap + __ctru_linear_heap_size) {
            extern void PortDbgX(const char* label, unsigned val);
            PortDbgX("[texdbg] tex w", width); PortDbgX("  h", height); PortDbgX("  data", (unsigned)d);
            PortDbgX("  slot", sCurTex);
        }
    }
#endif
    C3D_TexUpload(&sTexturePool[sCurTex], sTexBuf);
    C3D_TexFlush(&sTexturePool[sCurTex]);
    {
        int u;
        for (u = 0; u < 2; u++) { /* bound here: its registers (address, size) must be re-sent */
            if (sTexUnits[u] == sCurTex) {
                texBindUnit(u, &sTexturePool[sCurTex]);
                sTexBoundOk[u] = true;
            }
        }
    }
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
         return GPU_CLAMP_TO_EDGE;
    return (val & G_TX_MIRROR) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
}

static void gfx_citro3d_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    gPortC3dCalls[4]++;
    if (sRec) recOp(OP_SAMPLER, tile, linear_filter, (int)cms, (int)cmt);
    C3D_TexSetFilter(&sTexturePool[sTexUnits[tile]], linear_filter ? GPU_LINEAR : GPU_NEAREST, linear_filter ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&sTexturePool[sTexUnits[tile]], gfx_cm_to_opengl(cms), gfx_cm_to_opengl(cmt));
    texBindUnit(tile, &sTexturePool[sTexUnits[tile]]); /* the parameters are sent with the binding */
    sTexBoundOk[tile] = true;
}

/* PORT (2026-11-01): what the last updateDepth sent to the GPU (the draw log, gfx_pc.c) */
static float sDbgScale, sDbgOff;
static int sDbgBranch; /* 1 = W-buffer, 0 = z-buffer */
void gfx_citro3d_depth_info(char* buf, int n)
{
    snprintf(buf, n, "dep lin=%.2f br=%d t=%d u=%d dec=%d xlu=%d room=%d sc=%.9g off=%.9g", sDepthLinN, sDbgBranch,
             (int)sDepthTestOn, (int)sDepthUpdateOn, (int)sDepthDecal, (int)sDepthXlu, sDepthRoom, sDbgScale, sDbgOff);
}

static void updateDepth()
{
    C3D_DepthTest(sDepthTestOn, (sDepthRoom & 0x10000) ? GPU_ALWAYS : GPU_LEQUAL, sDepthUpdateOn ? GPU_WRITE_ALL : GPU_WRITE_COLOR); /* (0x10000: TEMPORARY Ground debug test) */
    /* PORT (2026-10-28): translucent surfaces (depth tested, not writing depth) drawn over the same geometry as an
     * opaque one get a small pull toward the camera. On the N64 the two compute identical depths; here they can go
     * through different vertex paths (CPU / GPU shaders) and differ in the last bits, so the overlay z-fought with what
     * it covers - a flickering moire on the water and ground outside the Deku Palace at some camera angles (user
     * report, video). Decals keep their larger offset. */
    if (sDepthLinN > 0.0f) {
        /* PORT (2026-10-28): the W-buffer. With z/w the PICA's 24-bit floats (16-bit mantissa) lose the depth of
         * distant surfaces: two surfaces a few units apart several hundred units away compare at random, and the
         * overlay over the ground outside the Deku Palace (and anything layered like it) flickered at some camera
         * angles, whatever small bias they got (user reports, three times). A W-buffer stores the distance itself:
         * about 0.03 units of error at any distance. Hardware depth = scale * zout + offset * w, zout = -(z + w) / 2
         * (gfx_pc.c), and zout + w = n (f - w) / (f - n): the near distance n up close, fading to 0 at the far plane, so
         *   depth = o (1 - e) w - (o d / n)(zout + w)  =  w / W_MAX  pulled toward the camera by up to d units + a share e of w
         * Decals and translucent overlays keep a pull, as before, now in units that mean the same at any distance. */
        const float o = 1.0f / DEPTH_W_MAX;
        float d = 0.0f, e = 0.0f, k;
        /* PORT (2026-10-30): the RDP lets an opaque surface win a tie against a translucent one (ZMODE_XLU passes only
         * when strictly nearer). The lily pads (opaque) sit exactly at the water's height, and the water (translucent,
         * not writing depth) drawn after them pulled toward the camera won the tie at random: pads flickering between
         * above and under the water (user reports). Translucent surfaces that only TEST depth are now pushed back a hair
         * instead; opaque and depth-writing surfaces keep their true depth (the z-buffer the game reads back for glows
         * and the sun is unchanged, a previous attempt that moved written depths let torch glows show through walls).
         * Decals keep their pull. */
        if (sDepthDecal) {
            /* PORT (2026-10-31): was 0.5 units + w / 4096 (0.6 at 500 units): a decal water surface drawn over the lily
             * pads, which stand a hair above it, won against them from the steep top-down views (pads "underwater",
             * user report, twice) but not from the ground. Just above the W-buffer's error (~0.03 + w / 131072). */
            d = 0.08f, e = 1.0f / 65536.0f;
        } else if (sDepthTestOn && !sDepthUpdateOn) {
            d = -0.1f, e = -1.0f / 32768.0f;
        }
        if ((sDepthRoom & 0xFFFF) && sDepthTestOn) {
            /* PORT (2026-10-31): the room's own geometry sits a hair farther back (0.3 units + w / 1024, 0.7 units at
             * 400 away), so what is drawn over it after it - actors - wins a near tie, as on the N64, where a later
             * surface passes up to its slope tolerance behind what is there and that tolerance grows with distance
             * and slant. The Deku Palace garden's ground is the room's ground plus ObjPurify's hill model
             * (object_dekucity_obj) in the same place: tied at random, the room's streaky texture (0x01d53a40 in the
             * texture dump) showed through in bands that came and went (user video, dump). Same for anything else
             * resting on room surfaces (shadows, lily pads). */
            d -= 0.3f, e -= 1.0f / 1024.0f;
            if (sDepthUpdateOn && !sDepthDecal) {
                /* PORT (2026-11-01): layers of the room drawn on top of each other in the same plane (the Deku Palace
                 * garden: the ground, then the beds, then the grass) tie in depth; the N64 gives the tie to the one drawn
                 * later (RDP: an opaque surface passes at equal depth), so the layering never changes. Here the winner
                 * was whichever the rounding favoured, a pattern that changed with the slightest camera movement: the
                 * ground's texture showing through the beds in smooth, streaky bands that came and went (user video,
                 * draw log: one 300-triangle ground draw and ~15 overlapping layer draws). Each mesh entry (z_room.c
                 * order, and each state change inside an entry) now sits a hair nearer than the one before: 0.08 units,
                 * above the W-buffer's own error (~0.03 + w / 131072), for up to 48 steps (3.8 units at most: the N64's
                 * own depth step is 3 units at 1000 away). */
                int n = (sDepthRoom & 0xFFFF) > 48 ? 48 : (sDepthRoom & 0xFFFF);
                d += 0.08f * (float)(n - 48);
                if (sDepthRoom & 0x20000) d += 6.0f; /* TEMPORARY Ground debug test: pulled 6 units nearer */
            }
            if (!sDepthDecal && !sDepthUpdateOn) {
                /* PORT (2026-11-01): translucent room surfaces (the swamp water) lose a near tie against what is
                 * already there by up to the N64's depth step, which grows with the SQUARE of the distance (~3e-6 w^2
                 * units: 0.75 at 500 away, 3 at 1000; 15-bit z). The pad draw log (user, 11/01) shows the pads (opaque,
                 * written) at w 580-1130 under the water (XLU, tested, w 700-1050): w / 1024 is 1 unit at 1000, a third
                 * of what the N64 absorbs, so from the high views the water won over pads that rest in it, and not
                 * from low ones. A linear stand-in for the square, right at the distances these views have. */
                e -= 1.0f / 320.0f;
            }
        }
        k = o * d / sDepthLinN;
        sDbgBranch = 1, sDbgScale = -k, sDbgOff = o * (1.0f - e) - k;
        C3D_DepthMap(false, -k, o * (1.0f - e) - k);
        return;
    }
    sDbgBranch = 0, sDbgScale = -1.0f, sDbgOff = sDepthDecal ? -0.001f : (sDepthTestOn && !sDepthUpdateOn ? -0.00005f : 0);
    C3D_DepthMap(true, -1, sDepthDecal ? -0.001f : (sDepthTestOn && !sDepthUpdateOn ? -0.00005f : 0));
}

static int depth_lin_bits(void) {
    int b;
    memcpy(&b, &sDepthLinN, sizeof(b));
    return b;
}

void gfx_citro3d_set_zmode_xlu(bool xlu) {
    if (xlu == sDepthXlu) return;
    sDepthXlu = xlu;
    if (sRec) recOp(OP_DECAL, sDepthDecal, depth_lin_bits(), sDepthXlu, sDepthRoom);
    updateDepth();
}

void gfx_citro3d_set_depth_room(int room) {
    if (room == sDepthRoom) return;
    sDepthRoom = room;
    if (sRec) recOp(OP_DECAL, sDepthDecal, depth_lin_bits(), sDepthXlu, sDepthRoom);
    updateDepth();
}

void gfx_citro3d_set_depth_linear(float nearDist) {
    if (nearDist == sDepthLinN) return;
    sDepthLinN = nearDist;
    if (sRec) recOp(OP_DECAL, sDepthDecal, depth_lin_bits(), sDepthXlu, sDepthRoom);
    updateDepth();
}

static void gfx_citro3d_set_depth_test(bool depth_test) {
    if (sRec) recOp(OP_DTEST, depth_test, 0, 0, 0);
    sDepthTestOn = depth_test;
    updateDepth();
}

static void gfx_citro3d_set_depth_mask(bool z_upd) {
    if (sRec) recOp(OP_DMASK, z_upd, 0, 0, 0);
    sDepthUpdateOn = z_upd;
    updateDepth();
}

static void gfx_citro3d_set_zmode_decal(bool zmode_decal) {
    if (sRec) recOp(OP_DECAL, zmode_decal, depth_lin_bits(), sDepthXlu, sDepthRoom);
    sDepthDecal = zmode_decal;
    updateDepth();
}

/* The top-screen target is PORTRAIT (240x400, rotated): x/y and width/height swap (sm64 3DS port),
 * and the vertex setup writes (y, -x), so N64 x runs down the target's second axis: a rectangle at x
 * sits at 400 - (x + width) there. PORT (2026-09-25): without that mirror every sub-viewport/scissor
 * was mirrored horizontally (full-screen ones are symmetric, so only small ones showed it: the A
 * button drawn over the hearts, found by tools/statediff fbdiff). */
#define TOP_W 400

extern int Port3ds_IsOffscreen(void);

/* stereo (gfx_3ds.c): the last top-screen viewport/scissor as given (x, y, w, h), re-issued per eye in
 * normal-mode target coordinates; kept in every mode so a switch into stereo starts with the right ones */
static int sVp[4] = { 0, 0, 400, 240 }, sSc[4] = { 0, 0, 400, 240 };

static void scissor_hw(void);
static void setRemap(float a, float b);
/* the current viewport's remap (non-stereo, applied at each draw): identity unless the viewport was clamped */
static float sVpRemap[2] = { 1.0f, 0.0f };
#define setRemapVp(a, b) (sVpRemap[0] = (a), sVpRemap[1] = (b))

static void gfx_citro3d_set_viewport(int x, int y, int width, int height) {
    if (sRec) recOp(OP_VIEWPORT, x, y, width, height);
    if (Port3ds_IsOffscreen()) { /* 1x 320x240-space off-screen target (gfx_3ds.c) */
        setRemapVp(1.0f, 0.0f);
        C3D_SetViewport(y, 320 - (x + width), height, width);
        return;
    }
    sVp[0] = x, sVp[1] = y, sVp[2] = width, sVp[3] = height;
    x = TOP_W - (x + width);
    if (gGfx3DSMode == GFX_3DS_MODE_STEREO) {
        C3D_SetViewport(y, x, height, width);
    } else {
        /* PORT (2026-10-26): a viewport running off the screen's long axis (the widescreen HUD's left-edge anchoring
         * moves the hearts/magic/rupees viewport 40 pixels past the target's end) is clamped to the target, and the
         * shader's remap (out.y = y * a + w * b, as the stereo path's far viewports) puts the geometry where the
         * unclamped viewport would have: the left HUD group stayed at its 4:3 place (user report) */
        int fl = (gGfx3DSMode == GFX_3DS_MODE_NORMAL) ? 1 : 2;
        int fs = (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12) ? 2 : 1;
        int d0 = x * fl, dw = width * fl, lim = TOP_W * fl;
        int a0 = d0 < 0 ? 0 : d0, a1 = (d0 + dw > lim) ? lim : d0 + dw;
        if (a1 > a0 && (a0 != d0 || a1 != d0 + dw)) {
            int aw = a1 - a0;
            C3D_SetViewport(y * fs, a0, height * fs, aw);
            setRemapVp((float)dw / (float)aw, (float)(2 * (d0 - a0) + dw) / (float)aw - 1.0f);
        } else {
            C3D_SetViewport(y * fs, d0, height * fs, dw);
            setRemapVp(1.0f, 0.0f);
        }
    }
    /* PORT (2026-10-26): C3D_SetViewport turns the scissor OFF (citro3d base.c). gfx_pc.c only re-sends the scissor
     * when the scissor itself changes, so after any viewport change (the A button's, a menu's, each frame's first)
     * the 2D screen drew with no scissor: the 3D view's wide sides stayed visible in 4:3 mode (user report: "the
     * aspect ratio doesn't change to 4:3"), and every N64 scissor (letterbox, split views) was lost. Stereo draws set
     * the scissor per eye already (gfx_citro3d_draw). Off-screen targets keep their old behaviour. */
    scissor_hw();
}

/* the last top-screen scissor (sSc) to the GPU, in the current mode's target coordinates */
static void scissor_hw(void) {
    int x = TOP_W - (sSc[0] + sSc[2]), y = sSc[1], width = sSc[2], height = sSc[3];
    if (gGfx3DSMode == GFX_3DS_MODE_STEREO || gGfx3DSMode == GFX_3DS_MODE_NORMAL)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x, y + height, x + width);
    else if (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y * 2, x * 2, (y + height) * 2, (x + width) * 2);
    else if (gGfx3DSMode == GFX_3DS_MODE_WIDE)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x * 2, y + height, (x + width) * 2);
}

static void gfx_citro3d_set_scissor(int x, int y, int width, int height)
{
    if (sRec) recOp(OP_SCISSOR, x, y, width, height);
    if (Port3ds_IsOffscreen()) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, 320 - (x + width), y + height, 320 - x);
        return;
    }
    sSc[0] = x, sSc[1] = y, sSc[2] = width, sSc[3] = height;
    x = TOP_W - (x + width);
    if (gGfx3DSMode == GFX_3DS_MODE_STEREO) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x, y + height, x + width);
    } else if (gGfx3DSMode == GFX_3DS_MODE_NORMAL)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x, y + height, x + width);
    else if (gGfx3DSMode == GFX_3DS_MODE_AA_22 || gGfx3DSMode == GFX_3DS_MODE_WIDE_AA_12)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y * 2, x * 2, (y + height) * 2, (x + width) * 2);
    else if (gGfx3DSMode == GFX_3DS_MODE_WIDE)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, y, x * 2, y + height, (x + width) * 2);
}

/* PORT (2026-09-24): alpha test per draw, libultraship semantics. Opaque: off. Cutout without
 * blending (texture edge): keep alpha > 0.19 and draw opaque. Cutout with blending (threshold):
 * discard alpha < 8/256. Other translucent draws: discard alpha 0 only (keeps depth clean). */
static void applyAlphaTest(void)
{
    /* PORT PERF (2026-10-01): only when it changes - every C3D_ state call marks state dirty and citro3d
     * re-sends those GPU registers with the next draw (this ran for every draw, twice in 3D) */
    static int sLast = -1;
    u32 id = sShaderProgramPool[sCurShader].shader_id1;
    int want = !(id & SHADER_OPT_ALPHA) ? 0 : (id & SHADER_OPT_TEXTURE_EDGE) ? (sUseBlend ? 1 : 2) : 3;
    if (want == sLast)
        return;
    sLast = want;
    if (want == 0)
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if (want != 3)
        C3D_AlphaTest(true, GPU_GREATER, want == 1 ? 7 : 48);
    else
        C3D_AlphaTest(true, GPU_GREATER, 0);
}

/* PORT (2026-10-30): the N64 blender's ADDITIVE modes (P * A + MEM * 1: light shafts, glows, G_RM_ADD...). Only
 * "P * A + MEM * (1 - A)" blended before; additive draws went out opaque - the Deku Palace garden's sun shafts lay on
 * the ground as solid flat bands, popping in and out as their animation changed (user report, marked screenshots).
 * 0 = not additive, 1 = A from the combiner (source alpha), 2 = A = fog colour alpha (sBlendConstA). */
static int sBlendAdd, sBlendConstA;
static void applyBlend()
{
    if (sUseBlend && sBlendAdd == 1) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
        return;
    }
    if (sUseBlend && sBlendAdd == 2) {
        C3D_BlendingColor((u32)(sBlendConstA & 0xFF) << 24);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
        return;
    }
    if (sUseBlend)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

static void gfx_citro3d_set_use_alpha(bool use_alpha)
{
    if (sRec) recOp(OP_ALPHA, use_alpha, sBlendAdd, sBlendConstA, 0);
    sUseBlend = use_alpha;
    applyBlend();
}

void gfx_citro3d_set_blend_add(int mode, int constA) {
    if (mode == sBlendAdd && constA == sBlendConstA) return;
    sBlendAdd = mode;
    sBlendConstA = constA;
    if (sRec) recOp(OP_ALPHA, sUseBlend, sBlendAdd, sBlendConstA, 0);
    applyBlend();
}

/* tools/statediff per-draw attribution: gfx_pc.c sets an id per flush while the color readback runs;
 * every fragment that passes the depth test writes it into the (otherwise unused) stencil buffer. */
static int sDrawId;

void gfx_citro3d_set_draw_id(int id) {
    if (sRec) recOp(OP_DRAWID, id, 0, 0, 0);
    sDrawId = id;
}

static void applyDrawId(void) {
    static int sLast = -1;
    if (sDrawId == sLast) {
        return;
    }
    sLast = sDrawId;
    if (sDrawId != 0) {
        C3D_StencilTest(true, GPU_ALWAYS, sDrawId, 0xFF, 0xFF);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
    }
}

/* eye uniform (shader.v.pica): (shift at infinity, convergence w, pop-out limit, constant shift).
 * PORT (2026-09-30, v2 after hardware feedback "not deep enough, Link pops out in 2D rooms"): the
 * convergence follows Link's distance (gPortStereoFocusW, smoothed in gfx_3ds.c), so Link sits at the
 * screen plane, the world recedes behind him and closer things come forward a little (limit below);
 * the 2D HUD (w < 1.5) stays at the screen. Before: a fixed convergence of 80 put Link almost as deep
 * as the horizon (little relative depth) and nothing could come forward. */
#define STEREO_POPOUT_LIMIT (-0.10f) /* closest things come forward at most 10% of the full depth:
                                      * comfort guideline - keep almost everything behind the screen */
#define STEREO_ROOM_DEPTH 0.75f      /* pre-rendered room picture: behind Link, who is at ~0.5 there (the
                                      * convergence halves in those rooms, gfx_3ds.c) - he stands inside
                                      * the picture instead of in front of it (hardware feedback) */
extern float gPortStereoConv;        /* gfx_3ds.c: smoothed convergence distance */
static int sEyeLoc = -1;
static float sEyeCur[4] = { -1.0f, -1.0f, -1.0f, -1.0f };

/* depth mode from gfx_pc.c (G_NOOP tags, S2DEX backgrounds): 0 = by distance, 1 = infinity (sky),
 * 2 = pre-rendered room picture, 3 = flat at screen depth (the HUD) */
static int sStereoMode;

void gfx_citro3d_set_stereo_mode(int mode) {
    if (sRec) recOp(OP_STEREO, mode, 0, 0, 0);
    sStereoMode = mode;
}

static int sRemapLoc = -1;
static float sRemapCur[3] = { -1.0f, -1.0f, -1.0f };

static void setRemap(float a, float b) {
    extern int gPortRawRelax; /* gfx_pc.c speed rules: no NoN depth clamp, the PICA clips at the near plane */
    float zlim = gPortRawRelax ? 1.0e10f : 0.0f;
    if (sRemapLoc >= 0 && (a != sRemapCur[0] || b != sRemapCur[1] || zlim != sRemapCur[2])) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sRemapLoc, a, b, zlim, 0.0f);
        sRemapCur[0] = a;
        sRemapCur[1] = b;
        sRemapCur[2] = zlim;
    }
}

/* pre-rendered rooms in 3D: the room picture is shifted sideways for depth, which uncovered its edge (a
 * "void" strip beside shop rooms, hardware v17). Everything but the HUD is zoomed horizontally by the
 * shift plus a margin - picture and 3D actors alike, so they stay aligned (a slight FOV change). */
static float sRoomZoom(float shift) {
    extern int gPortPrerenderedFrame;
    return gPortPrerenderedFrame ? fabsf(shift * STEREO_ROOM_DEPTH) * 1.5f : 0.0f;
}

static void setEye(float shift) {
    float u[4];
    if (sEyeLoc < 0) {
        return;
    }
    extern int gPortStereoFlatSceneR; /* gfx_3ds.c: menus (file select) - flat, backgrounds at half depth */
    if (shift == 0.0f || sStereoMode == 3 || (gPortStereoFlatSceneR && sStereoMode == 0)) {
        /* mono, a flat screen-depth layer (the HUD), or a menu's panels */
        u[0] = u[1] = u[2] = u[3] = 0.0f;
    } else if (gPortStereoFlatSceneR) { /* a menu's sky / room background: behind the panels */
        u[0] = 0.0f, u[1] = 0.0f, u[2] = fabsf(shift * 0.5f), u[3] = shift * 0.5f;
    } else if (sStereoMode == 1) { /* sky: the full shift, independent of the skybox box's own w */
        u[0] = 0.0f, u[1] = 0.0f, u[2] = 0.0f, u[3] = shift;
    } else if (sStereoMode == 2) { /* flat picture (w = 1): constant shift, zoomed (see sRoomZoom) */
        u[0] = 0.0f, u[1] = 0.0f, u[2] = sRoomZoom(shift), u[3] = shift * STEREO_ROOM_DEPTH;
    } else {
        u[0] = shift, u[1] = 0.0f, u[2] = sRoomZoom(shift), u[3] = 0.0f; /* depth curve per vertex (gfx_pc.c) */
    }
    if (u[0] != sEyeCur[0] || u[1] != sEyeCur[1] || u[2] != sEyeCur[2] || u[3] != sEyeCur[3]) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sEyeLoc, u[0], u[1], u[2], u[3]);
        memcpy(sEyeCur, u, sizeof(u));
    }
}

/* submit `count` vertices of the current batch: DrawArrays from `first`, or DrawElements from `idx` -
 * once per eye in stereo (see the viewport/eye notes inside) */
static void submitDraw_impl(u32 count, u32 first, const u16* idx);
static void submitDraw(u32 count, u32 first, const u16* idx) {
    extern volatile unsigned char gPortProf;
    unsigned char prev = gPortProf;
    gPortProf = 17; /* PROF_SUBMIT (port_prof.h) */
    submitDraw_impl(count, first, idx);
    gPortProf = prev;
}
extern int gPortGDbg, gPortGpuVtx;
#define GPU_STRIDE 56
extern int PortGfx_DlogOn(void);
extern uint32_t PortGfx_DlogBatch(void);
extern void PortGfx_DlogLine(const char* s);
extern int PortGfx_DlogMulti(void);
extern int PortGfx_DlogFrame(void);
/* TEMPORARY: the draw as the GPU gets it - counts, both bound textures' real parameters (size, format, wrap, filter, lod,
 * pool scale) and a checksum of the texture memory as it is at this moment, the first vertices as raw words */
static void dlogTexGpu(char* b, size_t n, int unit) {
    const C3D_Tex* t = &sTexturePool[sTexUnits[unit]];
    uint32_t h = 2166136261u;
    const uint8_t* d = (const uint8_t*)t->data;
    size_t i, sz = t->size < 65536 ? t->size : 65536;
    uint32_t sum[4] = { 0, 0, 0, 0 };
    if (PortGfx_DlogMulti() && PortGfx_DlogFrame() != 1) sz = 0; /* (the multi-frame capture: checksums in its first frame only) */
    if (d == NULL) sz = 0;
    for (i = 0; i < sz; i++) h = (h ^ d[i]) * 16777619u;
    if (t->fmt == GPU_RGBA8) { /* (2026-11-11) the average texel as the GPU holds it: R G B A (stored A B G R) */
        for (i = 0; i + 3 < sz; i += 4) sum[0] += d[i + 3], sum[1] += d[i + 2], sum[2] += d[i + 1], sum[3] += d[i];
    }
    snprintf(b, n, " | t%d pool=%d %ux%u fmt=%d size=%u param=%08x lod=%04x/%u/%u scale=%.5f/%.5f data=%p crc=%08x avg=%u,%u,%u,%u", unit,
             sTexUnits[unit], (unsigned)t->width, (unsigned)t->height, (int)t->fmt, (unsigned)t->size, (unsigned)t->param,
             (unsigned)t->lodBias, (unsigned)t->maxLevel, (unsigned)t->minLevel, (double)sTexturePoolScaleS[sTexUnits[unit]],
             (double)sTexturePoolScaleT[sTexUnits[unit]], (const void*)t->data, (unsigned)h, (unsigned)(sz >= 4 ? sum[0] / (sz / 4) : 0),
             (unsigned)(sz >= 4 ? sum[1] / (sz / 4) : 0), (unsigned)(sz >= 4 ? sum[2] / (sz / 4) : 0), (unsigned)(sz >= 4 ? sum[3] / (sz / 4) : 0));
}
/* (TEMPORARY, 2026-11-11) a GPU-path draw as the GPU itself will compute it, from the vertex buffer and the uniforms as they
 * stand for this draw (palette rows, the near-plane limit): how much of it lands on the screen, its w / z / depth range, the
 * texture coordinates and shade (alpha = fog) it carries. The CPU-side log (P / M lines) says what was meant. */
static int sPalLoc;   /* (defined with the GPU path below) */
static int sRemapLoc; /* (defined with setRemap below) */
static float dlogClipArea(const float v[3][3]) { /* screen area (pixels of 400x240) of a clip-space (x, y, w) triangle */
    float poly[12][3], tmp[12][3];
    int n = 3, pl, i, k;
    double a = 0.0;
    for (i = 0; i < 3; i++) poly[i][0] = v[i][0], poly[i][1] = v[i][1], poly[i][2] = v[i][2];
    for (pl = 0; pl < 5 && n >= 3; pl++) {
        int m = 0;
        for (i = 0; i < n; i++) {
            const float* p = poly[i];
            const float* q = poly[(i + 1) % n];
            float dp = pl == 0 ? p[2] + p[0] : pl == 1 ? p[2] - p[0] : pl == 2 ? p[2] + p[1] : pl == 3 ? p[2] - p[1] : p[2] - 1e-3f;
            float dq = pl == 0 ? q[2] + q[0] : pl == 1 ? q[2] - q[0] : pl == 2 ? q[2] + q[1] : pl == 3 ? q[2] - q[1] : q[2] - 1e-3f;
            if (dp >= 0.0f && m < 12) {
                for (k = 0; k < 3; k++) tmp[m][k] = p[k];
                m++;
            }
            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 12) {
                float t = dp / (dp - dq);
                for (k = 0; k < 3; k++) tmp[m][k] = p[k] + t * (q[k] - p[k]);
                m++;
            }
        }
        memcpy(poly, tmp, sizeof(float) * 3 * m);
        n = m;
    }
    for (i = 0; n >= 3 && i < n; i++) {
        const float* p = poly[i];
        const float* q = poly[(i + 1) % n];
        a += (double)(p[0] / p[2] * 200.0f) * (q[1] / q[2] * 120.0f) - (double)(q[0] / q[2] * 200.0f) * (p[1] / p[2] * 120.0f);
    }
    return (float)(a < 0 ? -a : a) * 0.5f;
}
static void dlogGpuEye(u32 count, const u16* idx) {
    const uint8_t* base = (const uint8_t*)sVboBuffer;
    float uvlo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, uvhi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
    float wlo = 1e30f, whi = -1e30f, zlo = 1e30f, zhi = -1e30f, dlo = 1e30f, dhi = -1e30f, area = 0.0f, zlim;
    int clo[4] = { 255, 255, 255, 255 }, chi[4] = { 0, 0, 0, 0 }, plo = 255, phi = 0, nz = 0, nd = 0, nOn = 0;
    char c[400];
    u32 i;
    int k, r;
    if (sPalLoc < 0 || sRemapLoc < 0 || base == NULL) return;
    zlim = C3D_FVUnif[GPU_VERTEX_SHADER][sRemapLoc].z;
    for (i = 0; i + 2 < count; i += 3) {
        float cv[3][3], a;
        for (k = 0; k < 3; k++) {
            const uint8_t* v = base + (size_t)idx[i + k] * GPU_STRIDE;
            float pos[4], uv[4], dp[4], o[4];
            uint8_t sh[4], pi;
            memcpy(pos, v, 16);
            memcpy(uv, v + 16, 16);
            memcpy(dp, v + 32, 16);
            memcpy(sh, v + 48, 4);
            pi = v[52];
            for (r = 0; r < 4; r++) {
                if (uv[r] < uvlo[r]) uvlo[r] = uv[r];
                if (uv[r] > uvhi[r]) uvhi[r] = uv[r];
                if (sh[r] < clo[r]) clo[r] = sh[r];
                if (sh[r] > chi[r]) chi[r] = sh[r];
            }
            if (pi < plo) plo = pi;
            if (pi > phi) phi = pi;
            if (dp[0] != 0.0f || dp[1] != 0.0f || dp[2] != 0.0f) nd++;
            for (r = 0; r < 4; r++) { /* the shader: dp4 with the palette entry's rows (no skinned delta at 20 fps) */
                const C3D_FVec* u = &C3D_FVUnif[GPU_VERTEX_SHADER][sPalLoc + pi * 4 + r];
                o[r] = u->x * pos[0] + u->y * pos[1] + u->z * pos[2] + u->w * pos[3];
            }
            if (o[2] > zlim) o[2] = zlim; /* (min with remap.z: the NoN near-plane clamp) */
            if (o[3] < wlo) wlo = o[3];
            if (o[3] > whi) whi = o[3];
            if (o[3] > 0.0f) {
                float z = o[2] / o[3], dep = sDbgScale * o[2] + sDbgOff * o[3];
                if (z < zlo) zlo = z;
                if (z > zhi) zhi = z;
                if (dep < dlo) dlo = dep;
                if (dep > dhi) dhi = dep;
                if (z > 0.0f || z < -1.0f) nz++;
            }
            /* back to the N64 clip space of the P lines: out = (y', -x') */
            cv[k][0] = -o[1], cv[k][1] = o[0], cv[k][2] = o[3];
        }
        a = dlogClipArea((const float(*)[3])cv);
        area += a;
        nOn += a > 0.0f;
    }
    snprintf(c, sizeof(c), "GE tris=%u onscreen=%d area=%.0f w=%.1f..%.1f z/w=%.4f..%.4f(out %d) depth=%.5f..%.5f uv0=%.2f..%.2f,%.2f..%.2f "
             "uv1=%.2f..%.2f,%.2f..%.2f shade=%d..%d,%d..%d,%d..%d a=%d..%d pal=%d..%d moved=%d zlim=%g\n",
             (unsigned)(count / 3), nOn, (double)area, (double)wlo, (double)whi, (double)zlo, (double)zhi, nz, (double)dlo, (double)dhi,
             (double)uvlo[0], (double)uvhi[0], (double)uvlo[1], (double)uvhi[1], (double)uvlo[2], (double)uvhi[2], (double)uvlo[3],
             (double)uvhi[3], clo[0], chi[0], clo[1], chi[1], clo[2], chi[2], clo[3], chi[3], plo, phi, nd, (double)zlim);
    PortGfx_DlogLine(c);
}
static int sRawMode;  /* (defined with the raw path below) */
static int sCullMode; /* (defined with gfx_citro3d_gpu_cull below) */
static void dlogDraw(u32 count, u32 first, const u16* idx) {
    char a[256], b[256], c[900];
    int k;
    if (!PortGfx_DlogOn() || count < 3) return; /* PORT (2026-11-10, TEMPORARY): every draw, raw ones and the GPU state too */
    dlogTexGpu(a, sizeof(a), 0);
    dlogTexGpu(b, sizeof(b), 1);
    {
        u32 id = sShaderProgramPool[sCurShader].shader_id1;
        int want = !(id & SHADER_OPT_ALPHA) ? 0 : (id & SHADER_OPT_TEXTURE_EDGE) ? (sUseBlend ? 1 : 2) : 3;
        snprintf(c, sizeof(c), "G batch=%u id=%d cnt=%u first=%u idx=%d raw=%d gpu=%d dbg=%d mode=%d | cull=%d dt=%d dw=%d dec=%d xlu=%d room=%d "
                 "lin=%.2f br=%d sc=%g off=%g alpha=%d blend=%d add=%d vp=%d,%d,%d,%d sc=%d,%d,%d,%d remap=%g,%g sh=%08x%s%s\n",
                 (unsigned)PortGfx_DlogBatch(), sDrawId, (unsigned)count, (unsigned)first, idx != NULL, sRawMode, gPortGpuVtx, gPortGDbg,
                 (int)gGfx3DSMode, sCullMode, sDepthTestOn, sDepthUpdateOn, sDepthDecal, sDepthXlu, sDepthRoom, (double)sDepthLinN,
                 sDbgBranch, (double)sDbgScale, (double)sDbgOff, want, sUseBlend, sBlendAdd, sVp[0], sVp[1], sVp[2], sVp[3], sSc[0],
                 sSc[1], sSc[2], sSc[3], (double)sVpRemap[0], (double)sVpRemap[1], (unsigned)id, a, b);
    }
    PortGfx_DlogLine(c);
    if (idx != NULL && sVboBuffer != NULL && !sRawMode && gPortGpuVtx && PortGfx_DlogFrame() == 1) {
        dlogGpuEye(count, idx);
    }
    if (idx != NULL && sVboBuffer != NULL && !sRawMode && !PortGfx_DlogMulti()) { /* first indices and the vertex words they point at (stride unknown here: 56 B GPU layout) */
        for (k = 0; k < 6 && (u32)k < count; k++) {
            const uint32_t* w = (const uint32_t*)((const uint8_t*)sVboBuffer + (size_t)idx[k] * (gPortGpuVtx ? GPU_STRIDE : VTX_FLOATS * 4));
            snprintf(c, sizeof(c), "GV %u:%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)idx[k], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], gPortGpuVtx ? w[13] : 0u);
            PortGfx_DlogLine(c);
        }
    }
}
static void submitDraw_impl(u32 count, u32 first, const u16* idx) {
    dlogDraw(count, first, idx);
    applyAlphaTest();
    applyDrawId();
    if (gGfx3DSMode == GFX_3DS_MODE_STEREO && !Port3ds_IsOffscreen()) {
        /* both eyes from the same vertices: left half then right half of the stereo target */
        int e;
        for (e = 0; e < 2; e++) {
            int off = e * STEREO_EYE_OFFSET;
            int vx = TOP_W - (sVp[0] + sVp[2]), sx = TOP_W - (sSc[0] + sSc[2]);
            int vy = vx + off, vh = sVp[2];
            /* The PICA framebuffer is stored bottom-up: viewport long-axis offset 0 lands in the SECOND
             * half of the buffer in memory, which sEyeOut[1] sends to GFX_RIGHT. So e = 0 draws the RIGHT
             * eye and gets -shift (uncrossed disparity = the world behind the screen). Confirmed on
             * hardware: v10 (this sign) had depth behind the screen; v11 (flipped, after misreading the
             * stereo_fb.bin dump, whose first half is the LEFT eye) made everything pop out and hurt. */
            setEye(e == 0 ? -gPortStereoSep : gPortStereoSep);
            /* PICA viewport origins are signed 10-bit: a right-eye viewport starting at >= 512 on the long
             * axis (the A button's at 524) wrapped negative and vanished. Start it at 400 instead, taller,
             * and remap clip y in the shader so the geometry lands on the same pixels. */
            if (vy > 511) {
                int dy = vy - off;
                setRemap((float)vh / (float)(dy + vh), (float)dy / (float)(dy + vh));
                C3D_SetViewport(sVp[1], off, sVp[3], dy + vh);
            } else {
                setRemap(1.0f, 0.0f);
                C3D_SetViewport(sVp[1], vy, sVp[3], vh);
            }
            C3D_SetScissor(GPU_SCISSOR_NORMAL, sSc[1], sx + off, sSc[1] + sSc[3], sx + sSc[2] + off);
            if (idx != NULL) {
                C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx);
            } else {
                C3D_DrawArrays(GPU_TRIANGLES, first, count);
            }
        }
    } else {
        setEye(0.0f);
        setRemap(sVpRemap[0], sVpRemap[1]);
        if (idx != NULL) {
            C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx);
        } else {
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
        }
    }
}

#define VBO_BYTES (2 * 1024 * 1024)
#define VBO_VERTS (VBO_BYTES / (VTX_FLOATS * 4))

/* ---- PORT (2026-10-01): GPU vertex path (gpu_vtx=1, shader_gpu.v.pica) ----
 * Vertices in model space (struct GpuVtx, gfx_pc.c writes them), transformed by a palette of up to
 * GPU_PAL matrices in vertex-shader uniforms. Fog/stereo parameters and the cull mode are per draw. */
int gPortGpuVtx = 1;              /* settings gpu_vtx=0/1, fixed at init. PORT PERF (2026-10-02): the GPU path is the
                                   * default - as accurate as the CPU path (fbdiff) and faster on both consoles
                                   * (hardware v27: New 3DS walk 9-16 vs 15-22 ms, Old 3DS speed 9.9-10.4 vs 9.6
                                   * updates/s before frame skip) */
#define GPU_PAL 18
#define GPU_STRIDE 56             /* sizeof(GpuVtx) in gfx_pc.c */
#define GPU_VERTS (VBO_BYTES / GPU_STRIDE)
static int sPalLoc = -1, sFogpLoc = -1, sStpLoc = -1;
static int sReplayK = 2;          /* the replay being drawn: 0 = t 1/3, 1 = t 2/3, 2 = the logic frame */
static uint16_t sPalSlotNow[GPU_PAL]; /* the slot whose rows each palette entry holds (replay by copy) */
static u32 sPalTouched; /* written in this replay: palette entry e = bit e, stp = bit 31 (older ones are stale) */
static float sGpuConv, sGpuFog[3];
#define REC_PALS 4096
static uint16_t (*sOpPal)[GPU_PAL]; /* recorded palettes (slot ids), OP_GPUPAL v[0] = index, v[1] = count */
static int sOpPalN;
static float (*sOpParam)[5];        /* recorded fog/stereo/screen-linear parameters, OP_GPUPARAM v[0] = index */
static int sOpParamN;


/* front-end vertex (gfx_pc.c: x y z w u0 v0 u1 v1 r g b a stereo) -> the PICA layout: portrait target
 * (y, -x), N64 z negated, texcoords scaled into the power-of-two PICA texture of the bound units */
static inline void writeVertex(float* dst, const float* src, float s0, float t0, float s1, float t1) {
    dst[0] = src[1];
    dst[1] = -src[0];
    dst[2] = -src[2];
    dst[3] = src[3];
    dst[4] = src[4] * s0;
    dst[5] = 1 - (src[5] * t0);
    dst[6] = src[6] * s1;
    dst[7] = 1 - (src[7] * t1);
    dst[8] = src[8];
    dst[9] = src[9];
    dst[10] = src[10];
    dst[11] = src[11];
    dst[12] = src[12]; /* stereo offset (gfx_pc.c stereo_offset) */
}

static void gfx_citro3d_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (sRec) { /* the array path is not recorded (A/B bench only): this frame is not replayable */
        sRecOverflow = 1;
    }
    if (sBufIdx * VTX_FLOATS + buf_vbo_len > VBO_BYTES / 4)
    {
        printf("Poly buf over!\n");
        return;
    }
    /* Invariant: gfx_pc.c packs VTX_FLOATS per vertex. A mismatch means the front end and this backend
     * disagree on the vertex layout; reading with the wrong stride walks off the buffer. */
    if (buf_vbo_len != buf_vbo_num_tris * 3 * VTX_FLOATS) {
        static int sReported;
        if (sReported < 4) {
            extern void PortDbgX(const char* label, unsigned val);
            sReported++;
            PortDbgX("VTX-STRIDE MISMATCH buf_vbo_len", (unsigned)buf_vbo_len);
            PortDbgX("  tris", (unsigned)buf_vbo_num_tris);
        }
        return;
    }
    /* texcoords are normalized to the uploaded texture; scale into the power-of-two PICA texture */
    float s0 = sTexturePoolScaleS[sTexUnits[0]], t0 = sTexturePoolScaleT[sTexUnits[0]];
    float s1 = sTexturePoolScaleS[sTexUnits[1]], t1 = sTexturePoolScaleT[sTexUnits[1]];
    const float* src = buf_vbo;
    float* dst = &((float*)sVboBuffer)[sBufIdx * VTX_FLOATS];
    for (size_t i = 0; i < 3 * buf_vbo_num_tris; i++, src += VTX_FLOATS, dst += VTX_FLOATS) {
        writeVertex(dst, src, s0, t0, s1, t1);
    }
    submitDraw(buf_vbo_num_tris * 3, sBufIdx, NULL);
    sBufIdx += buf_vbo_num_tris * 3;
    {
        extern u32 gPortPerfTris, gPortPerfDraws;
        gPortPerfTris += buf_vbo_num_tris;
        gPortPerfDraws++;
    }
}

/* PORT PERF (2026-09-30): indexed batches. The front end (gfx_pc.c) writes each vertex once per batch
 * straight into the VBO in the final layout (a loaded N64 vertex shared by several triangles is reused
 * by index) and each triangle adds 3 indices; the batch is drawn with DrawElements. The array path
 * above wrote 3 vertices per triangle into a staging buffer and copied/re-arranged all of them here
 * (hardware profile: triangle handling ~16 us each on Old 3DS). */
#define IDX_CAP (96 * 1024)
static u16* sIdxBuf;
static u32 sIdxPos, sIdxStart;
static u32 sIdxPlain; /* indices of this batch pushed for anything but a CPU-cut piece (gfx_citro3d_idx_push) */

/* n vertices at the end of this frame's VBO; NULL when full (index of the first in *first) */
float* gfx_citro3d_vtx_reserve(u32 n, u32* first) {
    if (sVboBuffer == NULL || sBufIdx + n > VBO_VERTS) {
        return NULL;
    }
    *first = sBufIdx;
    sBufIdx += n;
    return &((float*)sVboBuffer)[*first * VTX_FLOATS];
}

void gfx_citro3d_vtx_write(float* dst, const float* src) {
    writeVertex(dst, src, sTexturePoolScaleS[sTexUnits[0]], sTexturePoolScaleT[sTexUnits[0]],
                sTexturePoolScaleS[sTexUnits[1]], sTexturePoolScaleT[sTexUnits[1]]);
}

/* the VBO base, capacity (vertices), the running index and the bound units' texcoord scales, so the
 * front end can write vertices inline (one call per batch instead of two per vertex) */
float* gfx_citro3d_vbo_info(int** pos, u32* cap, float scale[4]) {
    *pos = &sBufIdx;
    *cap = VBO_VERTS;
    scale[0] = sTexturePoolScaleS[sTexUnits[0]];
    scale[1] = sTexturePoolScaleT[sTexUnits[0]];
    scale[2] = sTexturePoolScaleS[sTexUnits[1]];
    scale[3] = sTexturePoolScaleT[sTexUnits[1]];
    return (float*)sVboBuffer;
}

/* one triangle's indices; 0 when the index buffer is full */
int gfx_citro3d_idx_push(u32 a, u32 b, u32 c) {
    if (sIdxBuf == NULL || sIdxPos + 3 > IDX_CAP) {
        return 0;
    }
    sIdxBuf[sIdxPos++] = (u16)a;
    sIdxBuf[sIdxPos++] = (u16)b;
    sIdxBuf[sIdxPos++] = (u16)c;
    sIdxPlain += 3;
    return 1;
}

/* the same for a piece of a triangle cut on the CPU (gfx_pc.c gpu_pack_clip_tri): three new vertices */
int gfx_citro3d_idx_push_piece(u32 a, u32 b, u32 c) {
    if (!gfx_citro3d_idx_push(a, b, c)) {
        return 0;
    }
    sIdxPlain -= 3;
    return 1;
}

/* PORT (2026-11-14, Milk Road wall): a GPU-path batch made only of the pieces of CPU-cut triangles (their indices count
 * up, three new vertices each) goes out as a plain array draw: drawn through the index buffer, Milk Road's cliff and
 * ground pieces vanished at some camera angles (the draw log showed their vertex data was right). 2026-11-12 to 11-14
 * this covered EVERY batch whose indices count up, and the GPU hung on Goron roll / Deku spin (removed on 11-14: no
 * hang, but the wall was back - user tests); pieces alone were drawn this way everywhere in those builds without a
 * hang. Recorded for the replays as OP_DRAWIDX v[2]. */
static void submitIndexed(u32 start, u32 n, int pieces) {
    const u16* ix = sIdxBuf + start;
    u32 i = 0;
    if (pieces && !sRawMode && gPortGpuVtx) {
        for (i = 1; i < n && ix[i] == (u16)(ix[0] + i); i++) {
        }
    }
    if (i == n) {
        submitDraw(n, ix[0], NULL);
    } else {
        submitDraw(n, 0, ix);
    }
}

/* draw the indices pushed since the last batch */
void gfx_citro3d_draw_indexed(void) {
    u32 n = sIdxPos - sIdxStart;
    int pieces = sIdxPlain == 0;
    sIdxPlain = 0;
    if (n == 0) {
        return;
    }
    if (sRec) { /* drawn by the replays (direct capture: also now) */
        recOp(OP_DRAWIDX, (int)sIdxStart, (int)n, pieces, 0);
        if (!sRecDirect) {
            sIdxStart = sIdxPos;
            return;
        }
    }
    submitIndexed(sIdxStart, n, pieces);
    sIdxStart = sIdxPos;
    {
        extern u32 gPortPerfTris, gPortPerfDraws;
        gPortPerfTris += n / 3;
        gPortPerfDraws++;
    }
}

extern void gfx_gpu_slot_rows(uint16_t slot, int k, float rows[4][4]);

static void gpuUploadPalette(const uint16_t* slots, int start, int n) {
    int i;
    float rows[4][4];
    if (sPalLoc < 0 || n <= 0 || start < 0 || start + n > GPU_PAL) {
        return;
    }
    for (i = 0; i < n; i++) {
        float* u = (float*)C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, sPalLoc + (start + i) * 4, 4);
        sPalSlotNow[start + i] = slots[i];
        sPalTouched |= 1u << (start + i);
        gfx_gpu_slot_rows(slots[i], sReplayK, rows);
        /* citro3d uniforms are stored (w, z, y, x) per vec4 */
        int r;
        for (r = 0; r < 4; r++) {
            u[r * 4 + 0] = rows[r][3];
            u[r * 4 + 1] = rows[r][2];
            u[r * 4 + 2] = rows[r][1];
            u[r * 4 + 3] = rows[r][0];
        }
    }
}

static void gpuUploadParams(const float p[5]) {
    /* p: fog mul, fog offset, fog on, stereo convergence, screen-linear mode */
    if (sFogpLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sFogpLoc, p[0], p[1], p[2], 1.0f / 255.0f);
    }
    if (sStpLoc >= 0) {
        /* y = 1 - t of the replay being drawn (skinned vertices' delta toward the previous frame) */
        float back = sReplayK == 0 ? (2.0f / 3.0f) : sReplayK == 1 ? (1.0f / 3.0f) : 0.0f;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sStpLoc, p[3], back, p[4], 0.0005f);
        sPalTouched |= 1u << 31;
    }
}

/* gfx_pc.c, before a GPU-path draw: palette entries start..start+n-1 (slot ids) added since the last one */
void gfx_citro3d_gpu_palette(const uint16_t* slots, int start, int n) {
    if (sRec) {
        if (sOpPalN < REC_PALS) {
            memcpy(sOpPal[sOpPalN], slots, sizeof(uint16_t) * n);
            recOp(OP_GPUPAL, sOpPalN++, start, n, 0);
        } else {
            sRecOverflow = 1;
        }
    }
    gpuUploadPalette(slots, start, n);
}

/* fog multiplier/offset (already folded for z' = -(z + w) / 2), fog on, stereo convergence, screen-linear
 * shading (shader_gpu.v.pica) */
void gfx_citro3d_gpu_params(float fogMul, float fogOff, float fogOn, float conv, float lin) {
    float p[5] = { fogMul, fogOff, fogOn, conv, lin };
    if (sRec) {
        if (sOpParamN < REC_PALS) {
            memcpy(sOpParam[sOpParamN], p, sizeof(p));
            recOp(OP_GPUPARAM, sOpParamN++, 0, 0, 0);
        } else {
            sRecOverflow = 1;
        }
    }
    gpuUploadParams(p);
}

/* N64 cull mode (G_CULL_BACK etc. >> 9: 0 none, 1 front, 2 back, 3 both) */
static int sCullMode;
void gfx_citro3d_gpu_cull(int mode) {
    if (sRec) recOp(OP_CULL, mode, 0, 0, 0);
    sCullMode = mode;
    /* The portrait mapping (y, -x) is a rotation (keeps the winding); the N64 front face is
     * counter-clockwise on screen. Both = everything culled: the CPU path drew nothing either. */
    C3D_CullFace(mode == 1 ? GPU_CULL_FRONT_CCW : mode == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
}

/* the GPU-path vertex buffer: base, capacity (vertices) and the running index */
void* gfx_citro3d_gpu_vbo(int** pos, u32* cap, float scale[4]) {
    *pos = &sBufIdx;
    *cap = GPU_VERTS;
    scale[0] = sTexturePoolScaleS[sTexUnits[0]];
    scale[1] = sTexturePoolScaleT[sTexUnits[0]];
    scale[2] = sTexturePoolScaleS[sTexUnits[1]];
    scale[3] = sTexturePoolScaleT[sTexUnits[1]];
    return sVboBuffer;
}

/* ---- PORT PERF (2026-10-04): raw vertex path (gfx_pc.c gPortRawVtx, shader_raw.v.pica) ----
 * The game's 16-byte N64 vertices go to the GPU as they are; the shader does the matrix, lights, texture coordinate
 * scale and fog. Same binary as the GPU path (uniforms shared by name, so the palette and eye/fog values carry over
 * a switch); its own vertex buffer and format. Draws switch between the two programs (rectangles, texgen and skinned
 * loads stay on the GPU path). */
#define RAW_VBO_BYTES (768 * 1024)
#define RAW_STRIDE 16
#define REC_RAWP 2048
typedef struct {
    float uvc0[4], uvc1[4], lit[4], lamb[3], ldir[4][3], lcol[4][3]; /* lit: shader_raw.v.pica's lit uniform */
} RawParams;
static shaderProgram_s sProgRaw;
static bool sProgRawInit;
static void* sRawVbo;
static int sRawIdx;
static int sRawMode;              /* the raw program is bound */
static int sUvc0Loc = -1, sUvc1Loc = -1, sLitLoc = -1, sLambLoc = -1, sLdirLoc = -1, sLcolLoc = -1;
static C3D_AttrInfo sAttrGpu, sAttrRaw;
static C3D_BufInfo sBufGpu, sBufRaw;
static RawParams* sOpRawP;
static int sOpRawPN;
static shaderProgram_s sProg[2];
static bool sProgInit[2];

u32 gPortPerfRawSwitches; /* perf report: program + vertex format switches between the raw and GPU paths */
static C3Df_Config sCfgGpu, sCfgRaw;
static void rawBind(int raw) {
    gPortPerfRawSwitches++;
    /* only the registers that differ between the two configurations are sent (c3d_fast.c) */
    C3Df_SelectConfig(raw);
    sRawMode = raw;
}

/* gfx_pc.c, between draws: switch the program and vertex format (the caller flushed) */
void gfx_citro3d_raw_mode(int raw) {
    raw = raw != 0;
    if (raw == sRawMode || !sProgRawInit) {
        return;
    }
    if (sRec) recOp(OP_RAWMODE, raw, 0, 0, 0);
    rawBind(raw);
}

int gfx_citro3d_raw_ready(void) {
    return sProgRawInit && sRawVbo != NULL && sOpRawP != NULL;
}

/* PORT PERF (2026-10-04): only the values that changed are set: each set uniform is re-sent with the next draw, and
 * the command buffer is linear memory, whose writes cost the most on an Old 3DS. These uniforms are the raw program's
 * own (nothing else sets them), so the last values sent stay valid across program switches and frames. */
static RawParams sRawSent;
static int sRawSentOk; /* 0: nothing sent yet; bit 1: uvc0/uvc1/lit, bit 2: the lights */
static void rawUpload(const RawParams* p) {
    int i;
    if (sUvc0Loc < 0) return;
    if (!(sRawSentOk & 1) || memcmp(p->uvc0, sRawSent.uvc0, sizeof(p->uvc0)) != 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvc0Loc, p->uvc0[0], p->uvc0[1], p->uvc0[2], p->uvc0[3]);
        memcpy(sRawSent.uvc0, p->uvc0, sizeof(p->uvc0));
    }
    if (!(sRawSentOk & 1) || memcmp(p->uvc1, sRawSent.uvc1, sizeof(p->uvc1)) != 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvc1Loc, p->uvc1[0], p->uvc1[1], p->uvc1[2], p->uvc1[3]);
        memcpy(sRawSent.uvc1, p->uvc1, sizeof(p->uvc1));
    }
    if (!(sRawSentOk & 1) || memcmp(p->lit, sRawSent.lit, sizeof(p->lit)) != 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLitLoc, p->lit[0], p->lit[1], p->lit[2], p->lit[3]);
        memcpy(sRawSent.lit, p->lit, sizeof(p->lit));
    }
    sRawSentOk |= 1;
    if (p->lit[0] != 0.0f && (!(sRawSentOk & 2) || memcmp(p->lamb, sRawSent.lamb, sizeof(p->lamb)) != 0 ||
                           memcmp(p->ldir, sRawSent.ldir, sizeof(p->ldir)) != 0 ||
                           memcmp(p->lcol, sRawSent.lcol, sizeof(p->lcol)) != 0)) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLambLoc, p->lamb[0], p->lamb[1], p->lamb[2], 0.0f);
        for (i = 0; i < 4; i++) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sLdirLoc + i, p->ldir[i][0], p->ldir[i][1], p->ldir[i][2], 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sLcolLoc + i, p->lcol[i][0], p->lcol[i][1], p->lcol[i][2], 0.0f);
        }
        memcpy(sRawSent.lamb, p->lamb, sizeof(p->lamb));
        memcpy(sRawSent.ldir, p->ldir, sizeof(p->ldir));
        memcpy(sRawSent.lcol, p->lcol, sizeof(p->lcol));
        sRawSentOk |= 2;
    }
}

/* (replay by copy, canonUniforms) the raw path's uniforms at fixed values, and the next raw draw sends its own */
static void rawCanonUniforms(void) {
    int i;
    if (sUvc0Loc < 0) {
        return;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvc0Loc, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvc1Loc, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLitLoc, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLambLoc, 0.0f, 0.0f, 0.0f, 0.0f);
    for (i = 0; i < 4; i++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLdirLoc + i, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLcolLoc + i, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    sRawSentOk = 0;
}

/* gfx_pc.c, before a raw draw: its texture coordinate coefficients and lights */
void gfx_citro3d_raw_params(const float uvc0[4], const float uvc1[4], const float lit[4], const float lamb[3],
                            const float ldir[4][3], const float lcol[4][3]) {
    RawParams p;
    memcpy(p.uvc0, uvc0, sizeof(p.uvc0));
    memcpy(p.uvc1, uvc1, sizeof(p.uvc1));
    memcpy(p.lit, lit, sizeof(p.lit));
    if (lit[0] != 0.0f) {
        memcpy(p.lamb, lamb, sizeof(p.lamb));
        memcpy(p.ldir, ldir, sizeof(p.ldir));
        memcpy(p.lcol, lcol, sizeof(p.lcol));
    }
    if (sRec) {
        if (sOpRawPN < REC_RAWP) {
            sOpRawP[sOpRawPN] = p;
            recOp(OP_RAWPARAM, sOpRawPN++, 0, 0, 0);
        } else {
            sRecOverflow = 1;
        }
    }
    rawUpload(&p);
}

/* the raw vertex buffer: base, capacity (vertices) and the running index */
void* gfx_citro3d_raw_vbo(int** pos, u32* cap) {
    *pos = &sRawIdx;
    *cap = RAW_VBO_BYTES / RAW_STRIDE;
    return sRawVbo;
}

static void rawInit(void) {
    if (sProgRawInit || sVShaderDvlb == NULL || sVShaderDvlb->numDVLE < 3) {
        return;
    }
    sRawVbo = linearAlloc(RAW_VBO_BYTES);
    sOpRawP = malloc(sizeof(RawParams) * REC_RAWP);
    if (sRawVbo == NULL || sOpRawP == NULL) {
        return;
    }
    shaderProgramInit(&sProgRaw);
    shaderProgramSetVsh(&sProgRaw, &sVShaderDvlb->DVLE[2]);
    sUvc0Loc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "uvc0");
    sUvc1Loc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "uvc1");
    sLitLoc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "lit");
    sLambLoc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "lamb");
    sLdirLoc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "ldir");
    sLcolLoc = shaderInstanceGetUniformLocation(sProgRaw.vertexShader, "lcol");
    /* N64 Vtx: s16 ob[3], u16 flag (the palette index here), s16 tc[2], u8 cn[4] */
    AttrInfo_Init(&sAttrRaw);
    AttrInfo_AddLoader(&sAttrRaw, 0, GPU_SHORT, 4);         // v0 = x, y, z, palette index
    AttrInfo_AddLoader(&sAttrRaw, 1, GPU_SHORT, 2);         // v1 = s, t
    AttrInfo_AddLoader(&sAttrRaw, 2, GPU_UNSIGNED_BYTE, 4); // v2 = colour, or normal + alpha
    BufInfo_Init(&sBufRaw);
    BufInfo_Add(&sBufRaw, sRawVbo, RAW_STRIDE, 3, 0x210);
    sCfgGpu.prog = &sProg[1], sCfgGpu.attr = &sAttrGpu, sCfgGpu.buf = &sBufGpu;
    sCfgRaw.prog = &sProgRaw, sCfgRaw.attr = &sAttrRaw, sCfgRaw.buf = &sBufRaw;
    C3Df_SetConfigs(&sCfgGpu, &sCfgRaw);
    sProgRawInit = true;
    {
        extern void PortDbgX(const char*, unsigned);
        PortDbgX("[gfx] raw vertex path ready", 1);
    }
}
static void gfx_citro3d_setup_mode(int gpu) {
    extern void PortDbgX(const char*, unsigned);
    C3D_AttrInfo* attrInfo;
    C3D_BufInfo* bufInfo;
    if (!sProgInit[gpu]) {
        shaderProgramInit(&sProg[gpu]);
        shaderProgramSetVsh(&sProg[gpu], &sVShaderDvlb->DVLE[gpu]);
        sProgInit[gpu] = true;
    }
    C3D_BindProgram(&sProg[gpu]);
    sEyeLoc = shaderInstanceGetUniformLocation(sProg[gpu].vertexShader, "eye");
    sRemapLoc = shaderInstanceGetUniformLocation(sProg[gpu].vertexShader, "remap");
    sPalLoc = sFogpLoc = sStpLoc = -1;
    if (gpu) {
        sPalLoc = shaderInstanceGetUniformLocation(sProg[gpu].vertexShader, "pal");
        sFogpLoc = shaderInstanceGetUniformLocation(sProg[gpu].vertexShader, "fogp");
        sStpLoc = shaderInstanceGetUniformLocation(sProg[gpu].vertexShader, "stp");
        if (sOpPal == NULL) {
            sOpPal = malloc(sizeof(*sOpPal) * REC_PALS);
            sOpParam = malloc(sizeof(*sOpParam) * REC_PALS);
        }
    }
    PortDbgX(gpu ? "[gfx] vertex path: GPU (gpu_vtx)" : "[gfx] vertex path: CPU", 1);
    PortDbgX("[gfx] uniforms pal<<24|fogp<<16|stp<<8|eye", ((unsigned)(sPalLoc & 0xFF) << 24) | ((unsigned)(sFogpLoc & 0xFF) << 16) |
                                                            ((unsigned)(sStpLoc & 0xFF) << 8) | (unsigned)(sEyeLoc & 0xFF));
    PortDbgX("[gfx] uniforms remap<<24|uvc0<<16|uvc1<<8|lit", ((unsigned)(sRemapLoc & 0xFF) << 24) | ((unsigned)(sUvc0Loc & 0xFF) << 16) |
                                                               ((unsigned)(sUvc1Loc & 0xFF) << 8) | (unsigned)(sLitLoc & 0xFF));
    PortDbgX("[gfx] uniforms lamb<<16|ldir<<8|lcol", ((unsigned)(sLambLoc & 0xFF) << 16) | ((unsigned)(sLdirLoc & 0xFF) << 8) |
                                                      (unsigned)(sLcolLoc & 0xFF));
    /* a new program: its uniforms start unset */
    sEyeCur[0] = sEyeCur[1] = sEyeCur[2] = sEyeCur[3] = -1.0f;
    sRemapCur[0] = sRemapCur[1] = sRemapCur[2] = -1.0f;
    setEye(0.0f);
    setRemap(1.0f, 0.0f);

    attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    if (gpu) { /* struct GpuVtx: pos[4] uv0[2] uv1[2] dpos[4] (floats), shade[4] idx (bytes) */
        AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 4);         // v0 = position (model space w=1, or clip space)
        AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);         // v1 = texcoord 0
        AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 4); // v2 = shade 0..255
        AttrInfo_AddLoader(attrInfo, 3, GPU_FLOAT, 2);         // v3 = texcoord 1
        AttrInfo_AddLoader(attrInfo, 4, GPU_UNSIGNED_BYTE, 4); // v4 = palette index + padding
        AttrInfo_AddLoader(attrInfo, 5, GPU_FLOAT, 4);         // v5 = skinned delta; w = 1: fog precomputed
        /* buffer order: pos(v0) uv0(v1) uv1(v3) dpos(v5) shade(v2) idx/pad(v4) */
        BufInfo_Add(bufInfo, sVboBuffer, GPU_STRIDE, 6, 0x425310);
        sAttrGpu = *attrInfo; /* (the raw path switches back to these) */
        sBufGpu = *bufInfo;
        sRawMode = 0;
    } else {
        AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 4); // v0=position
        AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1=texcoord
        AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 4); // v2=color
        AttrInfo_AddLoader(attrInfo, 3, GPU_FLOAT, 2); // v3=texcoord1
        AttrInfo_AddLoader(attrInfo, 4, GPU_FLOAT, 1); // v4=stereo offset
        BufInfo_Add(bufInfo, sVboBuffer, VTX_FLOATS * 4, 5, 0x42310); // pos, uv0, uv1, color, stereo
    }
    C3D_CullFace(GPU_CULL_NONE); /* the CPU path culls itself; the GPU path sets it per draw */
}

/* gfx_pc.c / 3ds_main.c, between frames only */
void gfx_citro3d_set_gpu_mode(int gpu) {
    gpu = gpu != 0;
    if (gpu != gPortGpuVtx) {
        gPortGpuVtx = gpu;
        gfx_citro3d_setup_mode(gpu);
        sTexBoundOk[0] = sTexBoundOk[1] = false; /* unit 0's projective type follows the vertex path */
    }
}

static void gfx_citro3d_init(void)
{
    sVShaderDvlb = DVLB_ParseFile((u32*)shader_shbin, (u32)(shader_shbin_end - shader_shbin));
    // Create the VBO (vertex buffer object)
    sVboBuffer = linearAlloc(VBO_BYTES);
    sIdxBuf = linearAlloc(IDX_CAP * sizeof(u16));
#ifdef PORT_TEX_DEBUG
    { extern void PortDbgX(const char* label, unsigned val);
      PortDbgX("[texdbg] idx buf", (unsigned)(uintptr_t)sIdxBuf); PortDbgX("  vbo", (unsigned)(uintptr_t)sVboBuffer);
      PortDbgX("  raw vbo", (unsigned)(uintptr_t)sRawVbo); }
#endif
    gfx_citro3d_setup_mode(gPortGpuVtx != 0);
    rawInit();
    C3D_DepthMap(true, -1.0f, 0);
    C3D_DepthTest(false, GPU_LEQUAL, GPU_WRITE_ALL);
    C3D_AlphaTest(true, GPU_GREATER, 0x00);
}

/* (TEMPORARY, 2026-11-11) this frame's buffer use so far: GPU-path vertices, raw vertices, triangles (each with its cap) */
void gfx_citro3d_usage(u32 out[6]) {
    out[0] = (u32)sBufIdx, out[1] = GPU_VERTS;
    out[2] = (u32)sRawIdx, out[3] = RAW_VBO_BYTES / RAW_STRIDE;
    out[4] = sIdxPos / 3, out[5] = IDX_CAP / 3;
}

static void gfx_citro3d_start_frame(void) {
    sBufIdx = 0;
    sRawIdx = 0;
    sIdxPos = sIdxStart = 0;
    sIdxPlain = 0;
}

/* PORT (2026-09-30): the vertex buffer is cached linear memory written by the CPU every frame; the GPU
 * reads RAM. Nothing flushed it, so on hardware the GPU could draw stale vertices still sitting in the
 * data cache (the New 3DS has a 2 MB L2): intermittent hardware-only glitches - a ghost of a previous
 * frame's Link in the pause preview, stray pixel lines. Azahar has no CPU cache model, so it never
 * showed. Called by gfx_3ds.c right before C3D_FrameEnd submits the frame's commands. */
extern int gPortGDbg;
void gfx_citro3d_flush_vbo(void) {
    if (gPortGDbg == 20 || gPortGDbg == 22 || gPortGDbg == 23) { /* TEMPORARY Ground debug test: every byte of every vertex/index buffer, whatever the counters say */
        Port3ds_CacheFlush(sVboBuffer, VBO_BYTES);
        Port3ds_CacheFlush(sIdxBuf, IDX_CAP * sizeof(u16));
        Port3ds_CacheFlush(sRawVbo, RAW_VBO_BYTES);
        return;
    }
    if (sBufIdx > 0) {
        Port3ds_CacheFlush(sVboBuffer, sBufIdx * (gPortGpuVtx ? GPU_STRIDE : VTX_FLOATS * sizeof(float)));
    }
    if (sIdxPos > 0) {
        Port3ds_CacheFlush(sIdxBuf, sIdxPos * sizeof(u16));
    }
    if (sRawIdx > 0) {
        Port3ds_CacheFlush(sRawVbo, sRawIdx * RAW_STRIDE);
    }
}

/* PORT PERF (2026-10-05): replay by copy. An update's shown frames differ only in the palette matrices
 * (gfx_gpu_slot_rows of the frame's t) and the skinned-vertex blend (stp.y): the same draws, states, textures and
 * vertices. So the walk draws the first of them itself (gfx_citro3d_rec_direct) as a self-contained command stream
 * (c3d_fast.c C3Df_CaptureBegin re-sends all state at its start), the positions of every palette row and stp upload in
 * it are noted, and each further frame is a copy of those words with only those patched - instead of re-running the
 * draw log through citro3d (a third of a walk per frame). The log is still recorded: it is replayed whenever a copy
 * could differ (another render target or mode - 3D switched -, a full command buffer, a capture that did not fit) and
 * when the walk cannot draw directly (CPU vertex path). settings replay_copy=0 turns copies off. */
int gPortReplayCopy = 1;
int gPortReplayCopyCheck; /* settings replay_copy_check=1: the log is replayed (from a full state, as captured) and its
                           * command words compared with the patched copy's - any difference is a bug */
u32 gPortPerfReplayCopies, gPortPerfReplayCopyWords, gPortPerfCopyChecked, gPortPerfCopyBad;
u32 gPortCopyBadInfo[6]; /* the first mismatch: offset, words (log, copy), stream lengths (log << 16 | copy, x/4) */
typedef struct {
    u32 off;    /* word 0 of the vec4, from the start of the stream */
    u16 slot;   /* palette: the slot whose row it holds */
    u8 row;     /* palette: row 0-3 */
    u8 flags;   /* 1: the command header follows word 0 (first vec4 of a command), 2: stp (patch y = word 2),
                 * 4: stale - sent with the frame's full state before the frame wrote it (no draw reads it) */
} CapPatch;
#define CAP_PATCHES 16384
enum { CAP_NONE, CAP_ARMED, CAP_RUNNING, CAP_READY };
static int sCapState, sCapOverflow, sCapPN, sCapMode;
static CapPatch* sCapP;
static u32 *sCapWords, *sCapBase, *sCapCmdBuf;
static u32 sCapN, sCapCap, sCapTris, sCapDraws;
static const void* sCapTarget;
extern const void* Port3ds_DrawTargetId(void);
extern const float* gfx_gpu_slot_row_ptr(uint16_t slot, int k);

/* c3d_fast.c: a float-uniform run [first, first + count) was written at `data` */
static void capUnifRun(const u32* data, int first, int count) {
    int v, v0, v1;
    if (sStpLoc >= first && sStpLoc < first + count) {
        v0 = sStpLoc - first, v1 = v0 + 1; /* (stp sits apart from the palette: handled on its own below) */
    } else {
        v0 = v1 = 0;
    }
    for (v = v0; v < v1; v++) {
        int w = v * 4, wi = w & 255;
        if (sCapPN >= CAP_PATCHES) {
            sCapOverflow = 1;
            return;
        }
        sCapP[sCapPN].off = (u32)(data + (w >> 8) * 258 + (wi ? wi + 1 : 0) - sCapBase);
        sCapP[sCapPN].slot = 0, sCapP[sCapPN].row = 0;
        sCapP[sCapPN].flags = 2 | (wi == 0 ? 1 : 0) | ((sPalTouched >> 31) ? 0 : 4);
        sCapPN++;
    }
    v0 = sPalLoc - first, v1 = sPalLoc + GPU_PAL * 4 - first;
    v0 = v0 < 0 ? 0 : v0, v1 = v1 > count ? count : v1;
    for (v = v0; v < v1; v++) {
        int u = first + v - sPalLoc, w = v * 4, wi = w & 255;
        if (sCapPN >= CAP_PATCHES) {
            sCapOverflow = 1;
            return;
        }
        /* a run goes out as commands of 256 words (64 vec4s): [w0][header][w1..w255][pad] = 258 words */
        sCapP[sCapPN].off = (u32)(data + (w >> 8) * 258 + (wi ? wi + 1 : 0) - sCapBase);
        sCapP[sCapPN].slot = sPalSlotNow[u >> 2];
        sCapP[sCapPN].row = (u8)(u & 3);
        sCapP[sCapPN].flags = (wi == 0 ? 1 : 0) | ((sPalTouched >> (u >> 2)) & 1 ? 0 : 4);
        sCapPN++;
    }
}

/* a captured or replayed frame starts from fixed values of the uniforms each frame sets before use (palette, fog/stp,
 * raw-path coefficients and lights), not from what the previous frame left: the full-state upload at its start is then
 * the same words whichever frame came before (the walk follows the previous update, a replay follows the walk) */
static void canonUniforms(void) {
    if (sPalLoc >= 0) {
        memset(C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, sPalLoc, GPU_PAL * 4), 0, sizeof(float) * 16 * GPU_PAL);
    }
    if (sFogpLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sFogpLoc, 0.0f, 0.0f, 0.0f, 1.0f / 255.0f); /* (w: the shade byte scale) */
    }
    if (sStpLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sStpLoc, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    rawCanonUniforms();
}

static int capBegin(void) {
    extern u32 gPortPerfTris, gPortPerfDraws;
    if (sCapP == NULL) {
        sCapP = malloc(sizeof(CapPatch) * CAP_PATCHES);
        if (sCapP == NULL) {
            return 0;
        }
    }
    sCapPN = 0, sCapOverflow = 0;
    sPalTouched = 0;
    canonUniforms();
    C3Df_CaptureBegin(capUnifRun);
    sCapCmdBuf = gpuCmdBuf;
    sCapBase = gpuCmdBuf + gpuCmdBufOffset;
    sCapTris = gPortPerfTris, sCapDraws = gPortPerfDraws;
    sCapState = CAP_RUNNING;
    return 1;
}

static void capEnd(void) {
    extern u32 gPortPerfTris, gPortPerfDraws;
    u32 n = (u32)(gpuCmdBuf + gpuCmdBufOffset - sCapBase);
    C3Df_CaptureEnd();
    sCapState = CAP_NONE;
    if (sCapOverflow || gpuCmdBuf != sCapCmdBuf) { /* (no split can happen inside a replay; checked anyway) */
        return;
    }
    if (n > sCapCap) {
        u32 cap = n + n / 4 + 1024;
        u32* w = realloc(sCapWords, cap * sizeof(u32));
        if (w == NULL) {
            return;
        }
        sCapWords = w, sCapCap = cap;
    }
    memcpy(sCapWords, sCapBase, n * sizeof(u32));
    sCapN = n;
    sCapTris = gPortPerfTris - sCapTris, sCapDraws = gPortPerfDraws - sCapDraws;
    sCapTarget = Port3ds_DrawTargetId();
    sCapMode = gGfx3DSMode;
    sCapState = CAP_READY;
}

/* the captured words at dst (copied there first unless they are the live stream itself), patched for frame k */
static void capPatchK(u32* dst, int copy, int k) {
    const CapPatch *p, *end;
    union {
        float f;
        u32 u;
    } back;
    if (copy) {
        memcpy(dst, sCapWords, sCapN * sizeof(u32));
    }
    back.f = k == 0 ? (2.0f / 3.0f) : k == 1 ? (1.0f / 3.0f) : 0.0f; /* as gpuUploadParams */
    for (p = sCapP, end = sCapP + sCapPN; p < end; p++) {
        u32* w = dst + p->off;
        int g = p->flags & 1;
        if (p->flags & 4) {
            continue; /* stale: the canonical value (canonUniforms), the same for every frame */
        } else if (p->flags & 2) {
            w[2 + g] = back.u;
        } else {
            /* citro3d uniform words are (w, z, y, x): the row's coefficients 3, 2, 1, 0 */
            u32 r[4];
            memcpy(r, gfx_gpu_slot_row_ptr(p->slot, k) + p->row * 4, sizeof(r));
            w[0] = r[3], w[1 + g] = r[2], w[2 + g] = r[1], w[3 + g] = r[0];
        }
    }
}

static void capPatch(u32* dst) {
    capPatchK(dst, 1, sReplayK);
}

/* direct capture cut short (an off-screen render, a broken recording): the frame must show the logic frame after all.
 * What was drawn is still in the command buffer, unsubmitted: patched in place to t = 1, and citro3d's copies of the
 * palette and stp rewritten for t = 1, for the draws that follow. 0 if the words could not be patched. */
static int capToLogicFrame(void) {
    int ok = !sCapOverflow && gpuCmdBuf == sCapCmdBuf;
    int e;
    if (ok) {
        u32 n = (u32)(gpuCmdBuf + gpuCmdBufOffset - sCapBase);
        u32 keepN = sCapN;
        sCapN = n; /* (the patch records all lie inside) */
        capPatchK(sCapBase, 0, 2);
        sCapN = keepN;
    }
    sReplayK = 2;
    for (e = 0; e < GPU_PAL; e++) {
        gpuUploadPalette(&sPalSlotNow[e], e, 1);
    }
    if (sStpLoc >= 0) {
        float* u = (float*)C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, sStpLoc, 1);
        u[2] = 0.0f; /* (w, z, y, x): y = 1 - t */
    }
    return ok;
}

static int capUsable(void) {
    return Port3ds_DrawTargetId() == sCapTarget && gGfx3DSMode == sCapMode && gpuCmdBuf != NULL &&
           gpuCmdBufOffset + sCapN + 64 <= gpuCmdBufSize;
}

/* one in-between frame from the captured stream, patched for sReplayK; 0 = cannot (the caller replays the log) */
static int capEmit(void) {
    extern u32 gPortPerfTris, gPortPerfDraws;
    if (!capUsable()) {
        return 0;
    }
    capPatch(gpuCmdBuf + gpuCmdBufOffset);
    gpuCmdBufOffset += sCapN;
    C3Df_AfterCopy(sPalLoc, GPU_PAL * 4, sStpLoc);
    gPortPerfTris += sCapTris, gPortPerfDraws += sCapDraws;
    gPortPerfReplayCopies++;
    gPortPerfReplayCopyWords += sCapN;
    return 1;
}

/* the draw state the walk starts from, as the log's first entries: gfx_pc.c only sends what changed since the previous
 * frame, so without them a replay started from the state the walk ENDED in (its first draws could get another alpha
 * test, texture or cull mode than the walk's) */
static void recInitialState(void) {
    int t;
    recOp(OP_SHADER, sCurShader, 0, 0, 0);
    if (sConstN < REC_CONSTS) {
        sOpConsts[sConstN] = sConsts;
        recOp(OP_CONSTS, sConstN++, 0, 0, 0);
    }
    for (t = 0; t < 2; t++) { /* the bound textures with their sampler bits as they are now */
        recOp(OP_TEXPARAM, t, sTexUnits[t], (int)sTexturePool[sTexUnits[t]].param, 0);
    }
    recOp(OP_DTEST, sDepthTestOn, 0, 0, 0);
    recOp(OP_DMASK, sDepthUpdateOn, 0, 0, 0);
    recOp(OP_DECAL, sDepthDecal, depth_lin_bits(), sDepthXlu, sDepthRoom);
    recOp(OP_ALPHA, sUseBlend, sBlendAdd, sBlendConstA, 0);
    recOp(OP_DRAWID, sDrawId, 0, 0, 0);
    recOp(OP_STEREO, sStereoMode, 0, 0, 0);
    recOp(OP_CULL, sCullMode, 0, 0, 0);
}

/* gfx_pc.c, right after gfx_citro3d_rec_begin: the walk also draws, as the update's first shown frame k, and its command
 * words are captured for the frames after it - no replay of the log for that frame (Old 3DS: about a third of the walk).
 * 0 = not possible here (the walk only records, as before). */
int gfx_citro3d_rec_direct(int k) {
    extern void Port3ds_ResetFrameViewport(void);
    if (!sRec || !gPortReplayCopy || !gPortGpuVtx || sPalLoc < 0) {
        return 0;
    }
    Port3ds_ResetFrameViewport(); /* (as each replayed frame starts) */
    if (!capBegin()) {
        return 0;
    }
    sReplayK = k;
    sRecDirect = 1;
    return 1;
}

void gfx_citro3d_rec_begin(void) {
    if (sOps == NULL) {
        sOps = malloc(sizeof(RecOp) * REC_OPS);
        sOpConsts = malloc(sizeof(struct GfxCombineConsts) * REC_CONSTS);
    }
    sOpN = sConstN = 0;
    sOpPalN = sOpParamN = 0;
    sOpRawPN = 0;
    sRecOverflow = sOps == NULL || sOpConsts == NULL || (gPortGpuVtx && (sOpPal == NULL || sOpParam == NULL));
    sRec = !sRecOverflow;
    sCapState = CAP_NONE;
    sRecDirect = 0;
    if (sRec) {
        recInitialState();
    }
}

/* returns 1 when the walk drew the update's first shown frame itself (direct capture): no replay for it */
int gfx_citro3d_rec_end(void) {
    extern int gPortReplayBroken;
    sRec = 0;
    if (sRecOverflow) {
        gPortReplayBroken = 1;
    }
    if (sRecDirect) {
        sRecDirect = 0;
        if (gPortReplayBroken) { /* no frames follow: this one shows the logic frame */
            C3Df_CaptureEnd();
            capToLogicFrame();
            sCapState = CAP_NONE;
        } else {
            capEnd(); /* READY, or NONE if the capture did not fit (the frames after it replay the log) */
        }
        return 1;
    }
    /* the next replay is the update's first shown frame: capture it for the others (GPU path: the vertex buffer is
     * the same for all of them) */
    sCapState = gPortReplayCopy && gPortGpuVtx && !gPortReplayBroken && sPalLoc >= 0 ? CAP_ARMED : CAP_NONE;
    return 0;
}

/* GPU path: which in-between frame the next replay draws (palette matrices, skinned deltas) */
void gfx_citro3d_replay_variant(int k) {
    sReplayK = k;
}

/* re-issue the recorded state calls and draws (one shown frame) */
void gfx_citro3d_replay(void) {
    extern void Port3ds_ResetFrameViewport(void);
    int i, cap = 0, check = 0;
    u32* checkAt = NULL;
    /* every shown frame starts from the frame's own viewport, as the walk did (C3D_FrameDrawOn): the first replay used
     * to start from the walk's last one */
    Port3ds_ResetFrameViewport();
    sPalTouched = 0;
    if (gPortReplayCopy && gPortGpuVtx) {
        canonUniforms();
    }
    if (sCapState == CAP_READY) {
        if (gPortReplayCopyCheck && capUsable()) {
            check = 1;
            C3Df_CaptureBegin(NULL);
            checkAt = gpuCmdBuf + gpuCmdBufOffset;
        } else if (capEmit()) {
            return;
        } else {
            sCapState = CAP_NONE; /* the rest of this update replays the log */
        }
    } else if (sCapState == CAP_ARMED) {
        cap = capBegin();
    }
    if (sRawMode) {
        rawBind(0); /* the recording started on the GPU path's program (gfx_start_frame) */
    }
    for (i = 0; i < sOpN; i++) {
        const RecOp* o = &sOps[i];
        switch (o->op) {
            case OP_SHADER:
                gfx_citro3d_load_shader(&sShaderProgramPool[o->v[0]]);
                break;
            case OP_CONSTS:
                gfx_citro3d_set_combine_consts(&sOpConsts[o->v[0]]);
                break;
            case OP_TEX:
                gfx_citro3d_select_texture(o->v[0], (u32)o->v[1]);
                break;
            case OP_SAMPLER:
                gfx_citro3d_set_sampler_parameters(o->v[0], o->v[1] != 0, (uint32_t)o->v[2], (uint32_t)o->v[3]);
                break;
            case OP_DTEST:
                gfx_citro3d_set_depth_test(o->v[0] != 0);
                break;
            case OP_DMASK:
                gfx_citro3d_set_depth_mask(o->v[0] != 0);
                break;
            case OP_DECAL:
                memcpy(&sDepthLinN, &o->v[1], sizeof(sDepthLinN));
                sDepthXlu = o->v[2] != 0;
                sDepthRoom = o->v[3];
                gfx_citro3d_set_zmode_decal(o->v[0] != 0);
                break;
            case OP_VIEWPORT:
                gfx_citro3d_set_viewport(o->v[0], o->v[1], o->v[2], o->v[3]);
                break;
            case OP_SCISSOR:
                gfx_citro3d_set_scissor(o->v[0], o->v[1], o->v[2], o->v[3]);
                break;
            case OP_ALPHA:
                sBlendAdd = o->v[1], sBlendConstA = o->v[2];
                gfx_citro3d_set_use_alpha(o->v[0] != 0);
                break;
            case OP_DRAWID:
                gfx_citro3d_set_draw_id(o->v[0]);
                break;
            case OP_STEREO:
                gfx_citro3d_set_stereo_mode(o->v[0]);
                break;
            case OP_GPUPAL:
                gpuUploadPalette(sOpPal[o->v[0]], o->v[1], o->v[2]);
                break;
            case OP_GPUPARAM:
                gpuUploadParams(sOpParam[o->v[0]]);
                break;
            case OP_CULL:
                gfx_citro3d_gpu_cull(o->v[0]);
                break;
            case OP_RAWMODE:
                rawBind(o->v[0]);
                break;
            case OP_RAWPARAM:
                rawUpload(&sOpRawP[o->v[0]]);
                break;
            case OP_TEXPARAM:
                sTexturePool[o->v[1]].param = (u32)o->v[2];
                texBindUnit(o->v[0], &sTexturePool[o->v[1]]);
                sTexUnits[o->v[0]] = o->v[1];
                sTexBoundOk[o->v[0]] = true;
                break;
            case OP_DRAWIDX:
                submitIndexed((u32)o->v[0], (u32)o->v[1], o->v[2]);
                {
                    extern u32 gPortPerfTris, gPortPerfDraws;
                    gPortPerfTris += (u32)o->v[1] / 3;
                    gPortPerfDraws++;
                }
                break;
        }
    }
    if (cap) {
        capEnd();
    }
    if (check) {
        static u32* sCheck;
        static u32 sCheckCap;
        u32 n = (u32)(gpuCmdBuf + gpuCmdBufOffset - checkAt), j;
        C3Df_CaptureEnd();
        if (sCheckCap < sCapN) {
            free(sCheck);
            sCheckCap = sCapN + sCapN / 4;
            sCheck = malloc(sCheckCap * sizeof(u32));
            if (sCheck == NULL) {
                sCheckCap = 0;
                return;
            }
        }
        capPatch(sCheck);
        gPortPerfCopyChecked++;
        for (j = 0; j < n && j < sCapN && checkAt[j] == sCheck[j]; j++) {
        }
        if (j < n || n != sCapN) {
            if (gPortPerfCopyBad++ == 0) {
                /* the command holding word j: register << 16 | parameter index, and the uniform index last configured */
                u32 i2 = 0, reg = 0xFFFF, par = 0, unif = 0xFFFF;
                while (i2 + 1 < n) {
                    u32 hdr = checkAt[i2 + 1], extra = (hdr >> 20) & 0xFF, r = hdr & 0xFFFF, inc = hdr >> 31, q;
                    u32 len = 2 + extra + (extra & 1);
                    if (r == GPUREG_VSH_FLOATUNIFORM_CONFIG && i2 < j) {
                        unif = checkAt[i2] & 0xFF;
                    }
                    if (j < i2 + len) {
                        q = j == i2 ? 0 : j == i2 + 1 ? 0xFF : j - i2 - 1;
                        reg = r + (inc && q != 0xFF ? q : 0), par = q;
                        break;
                    }
                    i2 += len;
                }
                gPortCopyBadInfo[4] = (reg << 16) | (par & 0xFFFF);

                gPortCopyBadInfo[5] = unif;
                gPortCopyBadInfo[0] = j;
                gPortCopyBadInfo[1] = j < n ? checkAt[j] : 0xFFFFFFFFu;
                gPortCopyBadInfo[2] = j < sCapN ? sCheck[j] : 0xFFFFFFFFu;
                gPortCopyBadInfo[3] = (n << 16) | (sCapN & 0xFFFF);
            }
        }
    }
}

/* the walk meets something replay cannot cover (an off-screen render): draw what was recorded so far
 * now, at the logic frame's own positions, and stop recording - the rest of the frame draws directly */
void gfx_citro3d_rec_abort(void) {
    if (!sRec) {
        return;
    }
    sRec = 0;
    if (sRecDirect) { /* already drawn, as an in-between frame: becomes the logic frame */
        sRecDirect = 0;
        C3Df_CaptureEnd();
        capToLogicFrame();
        sCapState = CAP_NONE;
        sOpN = sConstN = 0;
        sOpPalN = sOpParamN = 0;
        sOpRawPN = 0;
        return;
    }
    sCapState = CAP_NONE;
    gfx_citro3d_replay();
    sOpN = sConstN = 0;
    sOpPalN = sOpParamN = 0;
    sOpRawPN = 0;
}

struct GfxRenderingAPI gfx_citro3d_api = {
    gfx_citro3d_z_is_from_0_to_1,
    gfx_citro3d_unload_shader,
    gfx_citro3d_load_shader,
    gfx_citro3d_create_and_load_new_shader,
    gfx_citro3d_lookup_shader,
    gfx_citro3d_shader_get_info,
    gfx_citro3d_set_combine_consts,
    gfx_citro3d_new_texture,
    gfx_citro3d_select_texture,
    gfx_citro3d_upload_texture,
    gfx_citro3d_set_sampler_parameters,
    gfx_citro3d_set_depth_test,
    gfx_citro3d_set_depth_mask,
    gfx_citro3d_set_zmode_decal,
    gfx_citro3d_set_viewport,
    gfx_citro3d_set_scissor,
    gfx_citro3d_set_use_alpha,
    gfx_citro3d_draw_triangles,
    gfx_citro3d_init,
    gfx_citro3d_start_frame
};

#endif
