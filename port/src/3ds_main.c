#include <malloc.h>
/*
 * 3ds_main.c — Nintendo 3DS entry point (libctru). Replaces pc_main.c/pc_gfx.c.
 * Boots the port runtime, drives the OoT gamestate loop, and reads the real
 * 3DS buttons into the controller shim. Rendering goes through the citro3d
 * backend (gfx3ds/gfx_citro3d.c) via the same gfx_pc interface proven on PC.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <math.h>

#include <PR/gbi.h>
#include "ultra64/sptask.h"
#include "gfx_pc.h"
#include "gfx_3ds.h"
#include "gfx_rendering_api.h"
#include "port_input.h"

/* N64 controller button bits (from include/controller.h) */
#define BTN_A_      0x8000
#define BTN_B_      0x4000
#define BTN_Z_      0x2000
#define BTN_START_  0x1000
#define BTN_DUP_    0x0800
#define BTN_DDOWN_  0x0400
#define BTN_DLEFT_  0x0200
#define BTN_DRIGHT_ 0x0100
#define BTN_L_      0x0020
#define BTN_R_      0x0010
#define BTN_CUP_    0x0008
#define BTN_CDOWN_  0x0004
#define BTN_CLEFT_  0x0002
#define BTN_CRIGHT_ 0x0001

extern struct GfxWindowManagerAPI gfx_3ds;
extern struct GfxRenderingAPI gfx_citro3d_api;

extern void Main(void* arg);
extern void Graph_ThreadEntry(void* arg);
extern void PortDma_Init(const char* romPath);

/* VI config globals the boot path expects (idle.c) */
extern unsigned char gViConfigModeType;
extern void* osViModeNtscLan1;

/* read by osContGetReadData via the shim */
static unsigned short s3dsButtons;
static signed char s3dsStickX, s3dsStickY;

unsigned short PortInput_GetPad(signed char* outX, signed char* outY) {
    if (outX) *outX = s3dsStickX;
    if (outY) *outY = s3dsStickY;
    return s3dsButtons;
}

/* PORT (2026-09-30): widescreen option (gfx_pc.c gPortWidescreen): SELECT toggles it (the N64 pad has
 * no SELECT), saved in sdmc:/3ds/mm/settings.txt. Off = the N64's 4:3 picture with side bars. */
#define PORT_SETTINGS_PATH sSettingsPath
/* PORT (2026-10-04): holding L while the game starts reads (and saves) sdmc:/3ds/mm/settings_b.txt instead, when it
 * exists: a second set of settings for hardware tests, chosen on the console without editing the SD card */
static const char* sSettingsPath = "sdmc:/3ds/mm/settings.txt";
static int sO3dsSimSetting;
/* bench=1: A/B benchmark - frames alternate between the indexed and the array vertex path (gfx_pc.c
 * gPortLegacyVbo); display-list time is accumulated per variant and logged with each report. Leave the
 * title screen's attract demo running (same content every boot). */
static int sBench;
/* prof=1: sampling profiler (port_prof.h) */
#include "port_prof.h"
#include "port_minimap.h" /* (also further down; the settings file needs the ZR mask slots, PORT_MASK_SLOT_FILES) */
volatile unsigned char gPortProf;
static int sProfOn;
static int sLogMute; /* the periodic perf report without a measurement switch on (PortGfx_RunTask) */
static int sGpuAB; /* settings gpu_ab=1: the vertex path alternates every 2 perf reports */
static int sAaAB;  /* settings aa_ab=1: anti-aliasing alternates every 2 perf reports (gfx_3ds.c) */
static int sCmdflushAB; /* settings cmdflush_ab=1: C3D_FrameEnd flush mode alternates every 2 reports */
static int sPresentAB;  /* settings present_ab=1: the present gate (gfx_3ds.c) alternates every 4 reports */
static int sOverlapAB;  /* settings overlap_ab=1: the CPU/GPU overlap (gfx_3ds.c gPortOverlap) alternates every 2 reports */
static int sRenderThreadFile = 1;    /* render_thread as settings.txt has it (written back as read) */
static int sRenderThreadSetting = 1; /* settings render_thread=0/1: draw on another core (Port3ds_RenderJob); on by default
                                      * since hardware v49 (New 3DS: 2D unchanged at 59.7-59.8 shown, 3D up to 59.8) */
static int sRenderThreadAB;      /* settings render_thread_ab=1: jobs alternate thread / inline every 2 reports */
static int sAudioShareAB;         /* settings audio_share_ab=1: the Old 3DS mixer's system-core share cycles 30/55/80 */
static int sRawAB;                /* settings raw_vtx_ab=1: the raw vertex path alternates every 2 perf reports */
static int sSpeedRules = 1; /* settings speed_rules (raw_relax): 0 off, 1 while frame skip is on (Old 3DS), 2 always (tests).
                        * Active: gfx_pc.c gPortRawRelax (near-camera geometry stays raw) and no screen-linear shading
                        * splits - the per-triangle CPU work that dominates Old 3DS drawing on hardware (v58 profile) */
static int sReplayCopyAB;
static int sSpeedRulesAB;         /* settings speed_rules_ab=1: speed rules 1 / 0 alternate every 2 reports (Old 3DS A/B) */         /* settings replay_copy_ab=1: replay by copy (gfx_citro3d.c) alternates every 2 reports */
static int sO3dsLayout;          /* settings o3ds_layout=1: the Old 3DS thread layout on a New 3DS (with o3ds_sim=1) */
int Port3ds_OnRenderThread(void);
static int sCstickCamera = 1; /* settings cstick=0: the New 3DS C-stick presses the C buttons, not the camera */
static int sGyroSensitivityPercent = 250;
int gPortStereoTest; /* settings stereo_test=1: 3D toggles every 2 s (gfx_3ds.c), freeze reproduction */
static volatile u32 sProfHist[PROF_COUNT];
static volatile int sProfRun;
static void Port3ds_ProfThread(void* arg) {
    (void)arg;
    while (sProfRun) {
        svcSleepThread(250000); /* 250 us */
        if (gPortProf < PROF_COUNT) sProfHist[gPortProf]++;
    }
}
static void Port3ds_ProfStart(void) {
    s32 prio = 0x30;
    if (!sProfOn) return;
    sProfRun = 1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* another core (the main thread is on core 0): 1 = the system core on Old 3DS, also present on New */
    if (threadCreate(Port3ds_ProfThread, NULL, 4096, prio - 2, 1, true) == NULL) {
        threadCreate(Port3ds_ProfThread, NULL, 4096, prio - 2, -2, true);
    }
}
/* fps60=1 turns the 60 fps frame interpolation on (docs/3ds-60fps-plan.md). PORT (2026-11-14): off on a first run (user
 * request); fps60 is always saved, so a player's own choice is kept. */
static int sInterp = 0;
/* PORT (2026-10-26): cutscene_dim=0 keeps the touch panel bright in cutscenes (default: dimmed, still usable) */
/* PORT (2026-11-08): the bottom screen's light in cutscenes, a step of kDimPct (3ds_main.c PanelDimWanted, DimPanel):
 * 0 = the screen off (its backlight too), 5 = no dimming. Settings cutscene_light (the old cutscene_dim=1/0 is 3/5). */
#define DIM_LEVELS 6
static const int kDimPct[DIM_LEVELS] = { 0, 10, 25, 50, 75, 100 };
static int sCutsceneDim = 3;
extern int gPortHudEdges; /* gfx_pc.c: widescreen HUD at the screen edges (settings hud_edges) */
extern int gPortHiDetail; /* gfx_pc.c: always the close-range level of detail (settings draw_distance=1) */
/* frameskip: -1 = automatic (on for the Old 3DS and o3ds_sim / perf_ab's Old 3DS phase), 0 = off, 1 = on.
 * See PortGfx_RunTask. */
static int sFrameSkip = -1;
static int sSplitSet; /* split_* in settings.txt: fixed thresholds, not chosen per console */
/* PORT PERF (2026-10-02): coarse splits also on a New 3DS while the logic frame is about to miss its refresh.
 * With the present gate an update's three frames take three consecutive vblanks, the logic frame no later
 * than the second after the update starts: game logic + walk + GPU must stay under ~32.9 ms (hardware v34:
 * ~33.5). The coarse thresholds save about 3 ms (split share 6.5% -> 1.6%, 101-scene error 5.92 -> 5.99).
 * Chosen from the measured margin with hysteresis; settings split_auto=0 keeps the fine ones. */
static int sSplitAuto = 1, sCoarseAuto;
static double sDeadlineMarginMs = 10.0; /* running average */
static unsigned sCoarseUpdates;         /* per report */
static double sWalkDoneSum;             /* logic frame done (GPU included) after the update's start, per report */
static unsigned sWalkDoneN;
static int sChose; /* this update chose its in-between frames (gameplay; not menus, readbacks) */
static u64 sBenchDl[2];
static u32 sBenchFrames[2]; /* o3ds_sim as read from settings.txt (not perf_ab's run-time toggling) */
static void Port3ds_SaveSettings(void) {
    extern int gPortWidescreen;
    FILE* f = fopen(PORT_SETTINGS_PATH, "w");
    if (f != NULL) {
        extern int gPortHudTop;
        fprintf(f, "widescreen=%d\n", gPortWidescreen ? 1 : 0);
        fprintf(f, "hud=%d\n", gPortHudTop ? 1 : 0);
        fprintf(f, "hud_edges=%d\n", gPortHudEdges ? 1 : 0);
        fprintf(f, "cutscene_light=%d\n", sCutsceneDim);
        fprintf(f, "draw_distance=%d\n", gPortHiDetail ? 1 : 0);
        { /* the ZR mask of each save file */
            int k;
            for (k = 0; k < PORT_MASK_SLOT_FILES; k++) fprintf(f, "mask_slot%d=%d\n", k, gPortMaskSlotFile[k]);
        }
        { /* GAMEPLAY options (port_minimap.h) */
            extern int gPortFastText, gPortSkipCutscenes, gPortInstantPutaway, gPortArrowCycle, gPortOwlPersist,
                gPortDoubleTime;
            extern int gPortPauseSave;
            fprintf(f, "pause_save=%d\n", gPortPauseSave);
            { extern int gPortGuardSight; fprintf(f, "guard_sight=%d\n", gPortGuardSight); }
            { /* CHEATS: only the ones switched on */
                extern int gPortCheats[];
                extern const int gPortCheatCount;
                int c;
                for (c = 0; c < gPortCheatCount; c++) {
                    if (gPortCheats[c]) fprintf(f, "cheat%d=%d\n", c, gPortCheats[c]);
                }
            }
            { /* the 2Ship2Harkinian options of GAMEPLAY pages 1-3: only the ones switched on */
                extern int gPortEnh[];
                extern const int gPortEnhCount;
                int c;
                for (c = 0; c < gPortEnhCount; c++) {
                    if (gPortEnh[c]) fprintf(f, "enh%d=%d\n", c, gPortEnh[c]);
                }
            }
            fprintf(f, "fast_text=%d\nskip_cutscenes=%d\ninstant_putaway=%d\narrow_cycle=%d\nowl_persist=%d\n"
                       "double_time=%d\n",
                    gPortFastText, gPortSkipCutscenes, gPortInstantPutaway, gPortArrowCycle, gPortOwlPersist,
                    gPortDoubleTime);
        }
        {
            /* measurement switches are written back as they were read (perf_ab toggles gPortO3dsSim at
             * run time; saving that state made the next boot start in Old-3DS speed) */
            extern int gPortPerfStagesOn, gPortPerfAB;
            if (sO3dsSimSetting) fprintf(f, "o3ds_sim=1\n");
            if (gPortPerfStagesOn) fprintf(f, "perf_stages=1\n");
            if (gPortPerfAB) fprintf(f, "perf_ab=1\n");
            if (sProfOn) fprintf(f, "prof=1\n");
            if (sGpuAB) fprintf(f, "gpu_ab=1\n");
            if (sAaAB) fprintf(f, "aa_ab=1\n");
            { extern int gPortCmdlistFlush; if (gPortCmdlistFlush && !sCmdflushAB) fprintf(f, "cmdlist_flush=1\n"); }
            if (sCmdflushAB) fprintf(f, "cmdflush_ab=1\n");
            { extern int gPortPresentGate; if (!gPortPresentGate && !sPresentAB) fprintf(f, "present_gate=0\n"); }
            if (sPresentAB) fprintf(f, "present_ab=1\n");
            { extern int gPortFlip; if (!gPortFlip) fprintf(f, "flip=0\n"); }
            { extern int gPortOverlap; if (!gPortOverlap && !sOverlapAB) fprintf(f, "overlap=0\n"); }
            if (sOverlapAB) fprintf(f, "overlap_ab=1\n");
            if (!sRenderThreadFile) fprintf(f, "render_thread=0\n"); /* (as read: the Old 3DS layout forces it off) */
            if (sRenderThreadAB) fprintf(f, "render_thread_ab=1\n");
            if (sO3dsLayout) fprintf(f, "o3ds_layout=1\n");
            { extern int gPortRawVtxWant; if (!gPortRawVtxWant && !sRawAB) fprintf(f, "raw_vtx=0\n"); }
            if (sRawAB) fprintf(f, "raw_vtx_ab=1\n");
            { extern int gPortReplayCopy; if (!gPortReplayCopy && !sReplayCopyAB) fprintf(f, "replay_copy=0\n"); }
            if (sReplayCopyAB) fprintf(f, "replay_copy_ab=1\n");
            if (sSpeedRulesAB) fprintf(f, "speed_rules_ab=1\n");
            if (sSpeedRules) fprintf(f, "speed_rules=%d\n", sSpeedRules);
            { extern int gPortAudioMargin; if (gPortAudioMargin >= 0) fprintf(f, "audio_margin=%d\n", gPortAudioMargin); }
            { extern int gPortAudioShare; if (gPortAudioShare != 55) fprintf(f, "audio_share=%d\n", gPortAudioShare); }
            if (sAudioShareAB) fprintf(f, "audio_share_ab=1\n");
            if (!sCstickCamera) fprintf(f, "cstick=0\n");
            if (sGyroSensitivityPercent != 250) {
                fprintf(f, "gyro_sensitivity=%d\n", sGyroSensitivityPercent);
            }
            { extern int gPortAA; if (gPortAA && !sAaAB) fprintf(f, "aa=1\n"); }
            { extern int gPortGpuVtx; if (!gPortGpuVtx && !sGpuAB) fprintf(f, "gpu_vtx=0\n"); }
            fprintf(f, "fps60=%d\n", sInterp); /* (always saved, so a change of the default never changes a player's choice) */
            if (sFrameSkip >= 0) fprintf(f, "frameskip=%d\n", sFrameSkip);
            { extern int gPortShadeSplit; if (!gPortShadeSplit) fprintf(f, "shade_split=0\n"); }
            if (!sSplitAuto) fprintf(f, "split_auto=0\n");
            { extern int gPortShadeLinear; if (gPortShadeLinear) fprintf(f, "shade_linear=1\n"); }
            if (sSplitSet) {
                extern float gPortSplitRatio, gPortSplitMinPx;
                extern int gPortSplitDepth;
                fprintf(f, "split_ratio=%d\nsplit_px=%d\nsplit_depth=%d\n", (int)(gPortSplitRatio * 100.0f + 0.5f),
                        (int)gPortSplitMinPx, gPortSplitDepth);
            }
        }
        fclose(f);
    }
}
/* PORT (2026-10-26): the entrance a pause save (ultra_shims.c) loads at, per file: the N64 save has no room for it.
 * Paired with the save's checksum, so an owl statue save made later is not taken for it. */
void Port3ds_PauseSaveWrite(int fileNum, unsigned short entrance, unsigned short checksum) {
    char path[64];
    FILE* f;
    snprintf(path, sizeof(path), "sdmc:/3ds/mm/pausesave%d.txt", fileNum);
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "entrance=%u\nchecksum=%u\n", (unsigned)entrance, (unsigned)checksum);
        fclose(f);
    }
}
int Port3ds_PauseSaveRead(int fileNum, unsigned short* entrance, unsigned short* checksum) {
    char path[64], line[64];
    unsigned e = 0xFFFFFFFFu, c = 0xFFFFFFFFu, v;
    FILE* f;
    snprintf(path, sizeof(path), "sdmc:/3ds/mm/pausesave%d.txt", fileNum);
    f = fopen(path, "r");
    if (f == NULL) return 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "entrance=%u", &v) == 1) e = v;
        if (sscanf(line, "checksum=%u", &v) == 1) c = v;
    }
    fclose(f);
    if (e > 0xFFFF || c > 0xFFFF) return 0;
    *entrance = (unsigned short)e;
    *checksum = (unsigned short)c;
    return 1;
}

static void Port3ds_LoadSettings(void) {
    extern int gPortWidescreen;
    char line[64];
    FILE* f = fopen(PORT_SETTINGS_PATH, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int v;
        {
            extern int gPortFastText, gPortSkipCutscenes, gPortInstantPutaway, gPortArrowCycle, gPortOwlPersist,
                gPortDoubleTime;
            if (sscanf(line, "fast_text=%d", &v) == 1) gPortFastText = v != 0;
            if (sscanf(line, "skip_cutscenes=%d", &v) == 1) gPortSkipCutscenes = v != 0;
            if (sscanf(line, "instant_putaway=%d", &v) == 1) gPortInstantPutaway = v != 0;
            if (sscanf(line, "arrow_cycle=%d", &v) == 1) gPortArrowCycle = v != 0;
            if (sscanf(line, "owl_persist=%d", &v) == 1) gPortOwlPersist = v != 0;
            if (sscanf(line, "double_time=%d", &v) == 1) gPortDoubleTime = v != 0;
            { extern int gPortPauseSave; if (sscanf(line, "pause_save=%d", &v) == 1) gPortPauseSave = v != 0; }
            { extern int gPortGuardSight; if (sscanf(line, "guard_sight=%d", &v) == 1) gPortGuardSight = v >= 0 && v <= 2 ? v : 0; }
            {
                extern int gPortCheats[];
                extern const int gPortCheatCount;
                int c;
                if (sscanf(line, "cheat%d=%d", &c, &v) == 2 && c >= 0 && c < gPortCheatCount) gPortCheats[c] = v >= 0 && v <= 3 ? v : 0;
            }
            {
                extern int gPortEnh[];
                extern const int gPortEnhCount;
                int c;
                if (sscanf(line, "enh%d=%d", &c, &v) == 2 && c >= 0 && c < gPortEnhCount) {
                    /* (the stepped rows keep their step; the others are on / off) */
                    int top = c == ENH_ACTOR_DISTANCE ? 4 : c == ENH_HUD_SIZE ? 6 : c == ENH_CRT ? 2 : 1;
                    gPortEnh[c] = top == 1 ? v != 0 : (v < 0 ? 0 : v > top ? top : v);
                }
            }
        }
        if (sscanf(line, "widescreen=%d", &v) == 1) {
            gPortWidescreen = v != 0;
        }
        if (sscanf(line, "hud=%d", &v) == 1) {
            extern int gPortHudTop;
            gPortHudTop = v != 0;
        }
        if (sscanf(line, "hud_edges=%d", &v) == 1) {
            gPortHudEdges = v != 0;
        }
        if (sscanf(line, "cutscene_dim=%d", &v) == 1) { /* (before 2026-11-08: dimmed or not) */
            sCutsceneDim = v != 0 ? 3 : DIM_LEVELS - 1;
        }
        if (sscanf(line, "cutscene_light=%d", &v) == 1) {
            sCutsceneDim = v < 0 ? 0 : v >= DIM_LEVELS ? DIM_LEVELS - 1 : v;
        }
        if (sscanf(line, "draw_distance=%d", &v) == 1) {
            gPortHiDetail = v != 0;
        }
        { /* the ZR mask of each save file (checked against its inventory in play) */
            int fileK;
            if (sscanf(line, "mask_slot%d=%d", &fileK, &v) == 2 && fileK >= 0 && fileK < PORT_MASK_SLOT_FILES) {
                gPortMaskSlotFile[fileK] = (v >= 0 && v <= 0xFF) ? v : 0xFF;
            }
        }
        if (sscanf(line, "mask_slot=%d", &v) == 1) { /* (before 2026-11-08: one for all files) */
            int k;
            for (k = 0; k < PORT_MASK_SLOT_FILES; k++) {
                if (gPortMaskSlotFile[k] == 0xFF) gPortMaskSlotFile[k] = (v >= 0 && v <= 0xFF) ? v : 0xFF;
            }
        }
        /* measurement switches (docs/3ds-60fps-plan.md P0), hand-edited in settings.txt:
         * o3ds_sim=1: New 3DS runs at the Old 3DS clock (268 MHz, no L2) - approximates an Old 3DS
         * perf_stages=1: per-stage frame timers in boot.log (one tick read per triangle) */
        if (sscanf(line, "o3ds_sim=%d", &v) == 1) {
            extern int gPortO3dsSim;
            gPortO3dsSim = v != 0;
            sO3dsSimSetting = v != 0;
        }
        if (sscanf(line, "bench=%d", &v) == 1) {
            sBench = v != 0;
        }
        if (sscanf(line, "prof=%d", &v) == 1) {
            sProfOn = v != 0;
        }
        if (sscanf(line, "cmdlist_flush=%d", &v) == 1) { extern int gPortCmdlistFlush; gPortCmdlistFlush = v != 0; }
        if (sscanf(line, "cmdflush_ab=%d", &v) == 1) { sCmdflushAB = v != 0; }
        if (sscanf(line, "present_gate=%d", &v) == 1) { extern int gPortPresentGate; gPortPresentGate = v != 0; }
        if (sscanf(line, "present_ab=%d", &v) == 1) { sPresentAB = v != 0; }
        if (sscanf(line, "flip=%d", &v) == 1) { extern int gPortFlip; gPortFlip = v != 0; }
        if (sscanf(line, "overlap=%d", &v) == 1) { extern int gPortOverlap; gPortOverlap = v != 0; }
        if (sscanf(line, "overlap_ab=%d", &v) == 1) { sOverlapAB = v != 0; }
        if (sscanf(line, "render_thread=%d", &v) == 1) { sRenderThreadSetting = sRenderThreadFile = v != 0; }
        if (sscanf(line, "render_thread_ab=%d", &v) == 1) { sRenderThreadAB = v != 0; }
        if (sscanf(line, "o3ds_layout=%d", &v) == 1) { sO3dsLayout = v != 0; }
        if (sscanf(line, "raw_vtx=%d", &v) == 1) { extern int gPortRawVtxWant; gPortRawVtxWant = v != 0; }
        if (sscanf(line, "raw_vtx_ab=%d", &v) == 1) { sRawAB = v != 0; }
        if (sscanf(line, "replay_copy_ab=%d", &v) == 1) { sReplayCopyAB = v != 0; }
        if (sscanf(line, "speed_rules_ab=%d", &v) == 1) { sSpeedRulesAB = v != 0; }
        if (sscanf(line, "raw_relax=%d", &v) == 1 || sscanf(line, "speed_rules=%d", &v) == 1) { sSpeedRules = v < 0 ? 0 : v > 2 ? 2 : v; }
        if (sscanf(line, "audio_margin=%d", &v) == 1 && v >= 0 && v <= 2048) { extern int gPortAudioMargin; gPortAudioMargin = v; }
        if (sscanf(line, "interp_dump_span=%d", &v) == 1 && v >= 1 && v <= 400) { extern int gPortInterpDumpSpan; gPortInterpDumpSpan = v; }
        if (sscanf(line, "interp_dump_at=%d", &v) == 1 && v >= 0) { extern int gPortInterpDumpAt; gPortInterpDumpAt = v; }
        if (sscanf(line, "fastswitch=%d", &v) == 1) { extern int gPortFastSwitch; gPortFastSwitch = v != 0; }
        if (sscanf(line, "replay_copy=%d", &v) == 1) { extern int gPortReplayCopy; gPortReplayCopy = v != 0; }
        if (sscanf(line, "replay_copy_check=%d", &v) == 1) { extern int gPortReplayCopyCheck; gPortReplayCopyCheck = v != 0; }
        if (sscanf(line, "audio_share=%d", &v) == 1) { extern int gPortAudioShare; gPortAudioShare = v; }
        if (sscanf(line, "audio_share_ab=%d", &v) == 1) { sAudioShareAB = v != 0; }
        if (sscanf(line, "raw_near=%d", &v) == 1) { extern int gPortRawNear; gPortRawNear = v; }
        if (sscanf(line, "raw_ratio=%d", &v) == 1 && v >= 10) { extern float gPortRawRatio; gPortRawRatio = v / 10.0f; }
        if (sscanf(line, "cstick=%d", &v) == 1) { sCstickCamera = v != 0; }
        if (sscanf(line, "gyro_sensitivity=%d", &v) == 1 && v >= 0 && v <= 1000) { /* 0 = gyro aiming off */
            sGyroSensitivityPercent = v;
        }
        if (sscanf(line, "flipdump=%d", &v) == 1) { extern int gPortFlipDump; gPortFlipDump = v != 0; }
        if (sscanf(line, "tjdump=%d", &v) == 1) { extern int gPortTjDumpFrame; gPortTjDumpFrame = v; }
        if (sscanf(line, "aa_ab=%d", &v) == 1) {
            sAaAB = v != 0;
        }
        if (sscanf(line, "aa=%d", &v) == 1) {
            extern int gPortAA;
            gPortAA = v != 0;
        }
        if (sscanf(line, "gpu_ab=%d", &v) == 1) { /* alternate the CPU / GPU vertex paths (hardware A/B) */
            sGpuAB = v != 0;
        }
        if (sscanf(line, "gpu_vtx=%d", &v) == 1) { /* GPU vertex path (gfx_citro3d.c), read before gfx init */
            extern int gPortGpuVtx;
            gPortGpuVtx = v != 0;
        }
        if (sscanf(line, "stereo_test=%d", &v) == 1) {
            gPortStereoTest = v != 0;
        }
        if (sscanf(line, "fps60=%d", &v) == 1) {
            sInterp = v;
        }
        if (sscanf(line, "frameskip=%d", &v) == 1) {
            sFrameSkip = v != 0;
        }
        if (sscanf(line, "split_ratio=%d", &v) == 1) { extern float gPortSplitRatio; gPortSplitRatio = v / 100.0f; sSplitSet = 1; }
        if (sscanf(line, "split_px=%d", &v) == 1) { extern float gPortSplitMinPx; gPortSplitMinPx = (float)v; sSplitSet = 1; }
        if (sscanf(line, "split_depth=%d", &v) == 1) { extern int gPortSplitDepth; gPortSplitDepth = v; sSplitSet = 1; }
        if (sscanf(line, "split_auto=%d", &v) == 1) { sSplitAuto = v != 0; }
        if (sscanf(line, "shade_split=%d", &v) == 1) {
            extern int gPortShadeSplit;
            gPortShadeSplit = v != 0;
        }
        if (sscanf(line, "shade_linear=%d", &v) == 1) {
            extern int gPortShadeLinear;
            gPortShadeLinear = v != 0;
        }
        if (sscanf(line, "perf_ab=%d", &v) == 1) {
            extern int gPortPerfAB;
            gPortPerfAB = v != 0;
        }
        if (sscanf(line, "perf_stages=%d", &v) == 1) {
            extern int gPortPerfStagesOn;
            gPortPerfStagesOn = v != 0;
        }
    }
    fclose(f);
}

#include "port_minimap.h"
extern const unsigned char* Port_GetItemIcon(int itemId);
extern int gPortWidescreen;

static int sTouchUi, sTouchUiRedraw, sTouchUiN3ds;
static u16* sFb;                                  /* where the panel draws: sPanelFb (or the minimap's shadow) */
static u16 sPanelFb[320 * 240] __attribute__((aligned(64))); /* the panel, off screen (Port3ds_TouchUiPoll) */
static int sFbDirty;

#define PRGB(r, g, b) (u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define RGB24(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

#define PANEL_ITEM_OCARINA_OF_TIME 0x00

/* PORT (2026-11-06): the MM3D-style touch panel, drawn as the user's saved design ("MM3D Bottom Screen": the first day,
 * the second day, the final hours; 320 x 240 with the game's textures 1:1). Top: the file-info box (rupees and small
 * keys, the name, 2 x 10 hearts, the magic meter), the day box (hours left, FIRST / SECOND / FINAL DAY), the clock, and a
 * slim bar across the three days ending in the moon. Left: View, Setup, Ocarina. Centre: the file select window with the
 * Items / Masks (ZR) / Map / Quest tabs; an item or mask dragged from its page onto X, Y, I (or a mask onto II) is
 * equipped there. Right: the X / Y / I / II sockets (II is the ZR mask slot). Blue; red on the final day. */
enum { P_VIEW, P_SETUP, P_OCARINA, P_CLEFT, P_CDOWN, P_CRIGHT, P_MASKS, P_TAB_ITEMS, P_TAB_MASKS, P_TAB_MAP, P_TAB_QUEST,
       P_COUNT };
enum { PG_ITEMS, PG_MASKS, PG_MAP, PG_QUEST };
typedef struct {
    s16 x, y, w, h;
    const char* label;
    u16 btn;  /* N64 bits while held; 0 = tap action */
    s8 page;  /* a tab: the page it shows */
} PanelPad;
static const PanelPad sPads[P_COUNT] = {
    { 2, 78, 36, 48, "View", BTN_CUP_, -1 },
    { 2, 134, 36, 48, "Setup", 0, -1 },
    { 2, 190, 36, 48, "Ocarina", 0, -1 },
    { 282, 120, 38, 43, "Y", BTN_CLEFT_, -1 },
    { 282, 163, 38, 43, "I", BTN_CDOWN_, -1 }, /* touch item slot I (+ ZL / D-pad down) */
    { 282, 78, 38, 42, "X", BTN_CRIGHT_, -1 },
    { 282, 206, 38, 34, "II", PORT_BTN_MASK, -1 }, /* the ZR mask slot */
    { 52, 82, 52, 22, "Items", 0, PG_ITEMS },
    { 106, 82, 52, 22, "Masks", 0, PG_MASKS },
    { 160, 82, 52, 22, "Map", 0, PG_MAP },
    { 214, 82, 52, 22, "Quest", 0, PG_QUEST },
};
static int sPage = PG_MAP; /* the centre window's page */
/* the file select window, its tabs, and the pages' area below the tabs */
#define WIN_X 40
#define WIN_Y 78
#define TAB_Y 86
#define CT_X 44
#define CT_Y 106
#define CT_W 232
#define CT_H 132
/* the Items / Masks grids: 6 x 4 cells (the design's cells, 28 wide and 6 apart) */
#define GRID_X 61
#define GRID_Y 107
#define GRID_CW 28
#define GRID_CH 26
#define GRID_GX 6
#define GRID_GY 4
#define HINT_Y 225 /* the line under a page */
/* the minimap inside the Map page */
#define MAP_X 61
#define MAP_Y 107
#define MAP_W 198
#define MAP_H 114
/* the Quest page's Bomber's Notebook button */
#define NOTE_X 61
#define NOTE_Y 190
#define NOTE_W 132
#define NOTE_H 24

/* where drawing may write (FillRect and the art / text blits; Px writes anywhere on the screen) */
static int sClipX0, sClipY0, sClipX1 = 320, sClipY1 = 240;

static inline void Px(int x, int y, u16 c) {
    if ((unsigned)x < 320 && (unsigned)y < 240) sFb[x * 240 + (239 - y)] = c;
}
static inline u16 PxGet(int x, int y) {
    return ((unsigned)x < 320 && (unsigned)y < 240) ? sFb[x * 240 + (239 - y)] : 0;
}
static inline int RoundDiv(int a, int b) { /* a / b to the nearest integer, b > 0 */
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}
static inline int Clamp8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
static void FillRect(int x, int y, int w, int h, u16 c) {
    int i, j, x0 = x < sClipX0 ? sClipX0 : x, y0 = y < sClipY0 ? sClipY0 : y;
    int x1 = x + w > sClipX1 ? sClipX1 : x + w, y1 = y + h > sClipY1 ? sClipY1 : y + h;
    for (i = x0; i < x1; i++) {
        for (j = y0; j < y1; j++) sFb[i * 240 + (239 - j)] = c;
    }
}
static inline int Div255(int v) { /* v / 255 for 0 <= v <= 255 * 255 (rounded), without a divide */
    return ((v + 128) * 257) >> 16;
}
static u16 Blend565(u16 bg, int r, int g, int b, int a) { /* a 0..255 */
    int br = (bg >> 11) << 3, bgc = ((bg >> 5) & 63) << 2, bb = (bg & 31) << 3;
    return PRGB(br + (r > br ? Div255((r - br) * a) : -Div255((br - r) * a)),
                bgc + (g > bgc ? Div255((g - bgc) * a) : -Div255((bgc - g) * a)),
                bb + (b > bb ? Div255((b - bb) * a) : -Div255((bb - b) * a)));
}
/* one pixel blended in (clipped) */
static inline void PxA(int x, int y, int r, int g, int b, int a) {
    u16* p;
    if (a <= 0 || x < sClipX0 || y < sClipY0 || x >= sClipX1 || y >= sClipY1) return;
    p = &sFb[x * 240 + (239 - y)];
    *p = a >= 255 ? PRGB(r, g, b) : Blend565(*p, r, g, b, a);
}
static void FillRectA(int x, int y, int w, int h, u32 rgb, int a) {
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) PxA(i, j, rgb >> 16, (rgb >> 8) & 255, rgb & 255, a);
}
static void FrameA(int x, int y, int w, int h, u32 rgb, int a) { /* a 1-pixel outline */
    FillRectA(x, y, w, 1, rgb, a);
    FillRectA(x, y + h - 1, w, 1, rgb, a);
    FillRectA(x, y + 1, 1, h - 2, rgb, a);
    FillRectA(x + w - 1, y + 1, 1, h - 2, rgb, a);
}
static void ClipSet(int x, int y, int w, int h) {
    sClipX0 = x < 0 ? 0 : x, sClipY0 = y < 0 ? 0 : y;
    sClipX1 = x + w > 320 ? 320 : x + w, sClipY1 = y + h > 240 ? 240 : y + h;
}
static void ClipReset(void) {
    sClipX0 = sClipY0 = 0, sClipX1 = 320, sClipY1 = 240;
}

/* ---- the theme: the file select's blue, red on the final day ---- */
static int sThemeRed; /* the final day (set at each whole redraw) */
static u32 ThemeTint(void) { /* what the game's file select art is tinted with (its window colour) */
    return sThemeRed ? RGB24(255, 110, 90) : RGB24(100, 150, 255);
}
static u32 ThemeTintHi(void) { /* pressed */
    return sThemeRed ? RGB24(255, 160, 140) : RGB24(160, 200, 255);
}
static u16 ColBg(void) {
    return sThemeRed ? PRGB(0x1a, 0x0b, 0x18) : PRGB(0x0b, 0x10, 0x33);
}
static u32 ColShadow(void) { /* the text's drop shadow */
    return sThemeRed ? RGB24(0x1a, 0x06, 0x0a) : RGB24(0x0a, 0x0f, 0x2a);
}
#define COL_WHITE RGB24(255, 255, 255)
#define COL_GOLD24 RGB24(0xff, 0xd3, 0x4d)
#define COL_HINT RGB24(0xdb, 0xe5, 0xff)
#define COL_OFF RGB24(150, 160, 190)
#define COL_INK RGB24(0x10, 0x18, 0x3a) /* the letters on the badges */

/* ---- the panel font ---- */
/* the panel font: Inter (Bold / SemiBold; SIL Open Font License 1.1, rsms.me/inter), rasterized at the sizes the panel
 * uses, 4-bit coverage, two pixels per byte (generated) */
typedef struct { u16 off; u8 w, h; s8 bx, by; u8 adv8; } PGlyph; /* by: top relative to the baseline; adv in 1/8 px */
typedef struct { const char* chars; u16 first; u8 asc; } PFont;
static const u8 kPFontBits[10960] = {
    119,119,102,34,86,104,164,88,10,85,109,201,40,145,188,213,115,160,1,16,61,195,137,100,44,194,101,137,77,211,1,16,75,66,112,75,73,16,0,69,
    0,0,147,181,7,35,181,44,144,73,176,63,84,166,215,91,185,102,69,5,2,176,103,7,96,88,1,192,81,118,74,59,89,147,58,84,216,4,0,1,
    16,5,96,92,198,4,80,100,145,107,64,100,2,16,177,11,4,128,132,11,0,44,195,150,89,179,59,149,89,60,195,110,52,179,11,48,179,11,48,59,
    177,82,133,1,193,28,48,158,181,59,177,50,149,6,194,66,120,75,178,3,243,10,179,73,147,172,217,0,147,93,179,117,0,122,178,33,103,59,178,44,
    178,133,68,186,179,165,88,60,178,155,228,1,208,6,128,12,32,74,0,59,178,119,133,60,194,164,104,75,179,75,194,163,120,91,185,83,118,60,177,100,
    0,101,70,0,67,131,48,0,2,23,196,109,48,2,150,91,181,0,0,91,181,33,0,59,129,3,199,89,32,91,144,50,224,10,80,5,0,11,0,2,
    137,48,43,33,164,148,171,122,165,86,90,148,170,182,59,34,32,3,154,64,8,224,0,202,64,44,104,6,203,208,180,12,48,156,196,148,119,155,196,148,
    74,156,197,44,199,8,112,144,179,0,9,112,144,44,199,0,157,212,9,66,208,148,14,9,66,208,157,213,0,156,179,148,0,156,178,148,0,156,179,156,
    179,148,0,156,177,148,0,148,0,28,199,8,112,128,179,106,40,112,194,44,216,0,148,12,25,64,193,156,190,25,64,193,148,12,16,148,148,148,148,148,
    0,194,0,194,0,194,145,193,107,144,148,91,9,109,16,158,160,9,107,64,148,60,0,148,0,148,0,148,0,148,0,156,178,158,5,243,157,74,211,153,
    141,179,149,217,179,148,181,179,154,11,57,211,179,151,187,57,74,211,148,47,48,44,216,8,112,194,179,9,88,112,195,44,216,0,156,195,148,88,156,179,
    148,0,148,0,44,216,8,112,194,179,9,88,119,211,44,220,0,0,49,156,195,148,89,156,212,148,194,148,105,59,179,135,35,43,194,82,105,75,180,157,
    217,7,112,7,112,7,112,7,112,148,13,148,13,148,13,134,29,43,198,180,12,54,129,208,29,89,0,202,64,7,224,0,179,122,14,8,106,195,176,73,
    202,135,1,218,125,64,12,116,241,0,105,60,0,203,64,7,208,1,218,96,135,45,16,150,29,18,215,112,9,209,0,74,0,4,160,0,139,217,1,210,
    8,112,44,0,157,183,89,118,118,118,118,90,32,11,16,117,3,144,11,0,162,119,43,43,43,43,119,45,24,55,170,144,55,91,160,22,225,166,209,139,
    209,148,0,155,210,149,119,149,119,155,194,76,176,147,49,147,49,59,176,0,118,92,198,179,150,179,150,92,198,74,160,170,196,163,32,76,177,25,37,128,
    189,38,112,103,6,112,92,198,179,134,165,166,41,166,74,177,147,0,155,193,148,149,148,133,148,133,81,0,148,148,148,148,5,16,0,10,64,164,10,64,
    164,44,16,148,0,149,194,157,96,156,144,148,211,148,148,148,148,148,154,201,176,148,179,193,148,178,193,148,178,193,154,193,149,149,148,133,148,133,76,192,
    147,132,147,132,59,161,155,210,149,119,149,119,155,194,148,0,33,0,92,198,179,150,179,150,92,198,0,118,0,17,155,73,80,148,9,64,92,160,153,48,
    39,225,106,160,100,12,192,118,7,96,76,0,148,133,148,133,148,149,76,197,194,133,118,193,43,176,13,112,194,225,216,125,121,92,140,97,241,226,133,193,
    29,128,45,144,148,194,178,133,103,193,28,160,12,80,107,0,123,224,7,112,45,16,157,177,5,80,209,28,8,128,12,0,85,88,88,88,88,88,88,88,
    17,99,3,176,28,0,181,43,6,48,24,18,101,181,76,76,76,59,2,59,60,149,43,133,20,49,7,86,86,236,236,12,11,0,192,192,174,206,131,131,
    144,0,80,1,174,178,107,117,98,236,48,1,172,135,119,108,42,236,64,5,0,10,160,10,32,41,147,102,0,7,114,160,0,0,10,38,112,0,102,41,
    147,2,160,10,161,9,198,0,29,59,0,12,212,0,76,196,160,151,62,144,59,218,192,60,43,20,1,32,150,14,18,208,76,2,208,14,32,119,49,7,
    128,30,0,225,12,48,226,45,8,112,36,131,44,228,56,166,1,32,0,65,0,11,32,60,237,128,11,32,0,97,0,75,102,49,108,194,2,59,2,176,
    103,10,48,208,58,7,96,178,0,9,219,21,192,137,136,4,200,128,76,92,8,144,157,178,61,199,108,3,192,60,3,192,60,27,217,22,128,181,0,29,
    32,11,112,11,128,7,253,214,26,218,19,80,166,1,220,16,1,167,104,9,145,173,162,0,111,48,29,211,10,92,52,176,195,141,223,192,0,195,31,221,
    67,176,0,93,204,32,32,152,72,9,129,173,177,9,219,21,192,84,139,205,40,176,121,91,8,144,174,194,157,223,64,2,225,0,152,0,30,16,8,128,
    1,225,0,26,203,37,176,166,11,220,22,178,167,137,8,162,172,178,27,234,23,160,167,137,9,162,204,185,54,10,97,174,160,2,75,0,2,59,44,2,
    0,40,88,83,0,0,32,23,214,62,113,1,157,113,0,24,112,61,221,112,0,0,61,221,128,18,0,1,217,32,0,92,128,75,180,42,48,0,60,198,
    100,61,0,168,6,160,2,32,8,112,0,122,183,16,11,114,22,192,89,77,202,133,133,162,74,72,133,162,74,86,89,77,187,194,12,114,18,0,1,122,
    184,0,0,235,0,4,190,16,9,123,96,14,55,176,95,221,242,168,0,183,110,220,54,144,121,110,189,38,144,122,105,5,198,237,229,6,222,128,78,33,
    213,137,0,0,137,0,0,77,17,197,7,222,128,110,236,64,105,3,225,105,0,180,105,0,180,105,3,225,110,220,64,110,221,86,144,0,110,221,38,144,
    0,105,0,6,237,213,110,221,70,144,0,105,0,6,237,209,105,0,6,144,0,6,222,128,61,33,213,136,0,0,136,10,232,77,33,198,7,222,144,105,
    0,166,105,0,166,110,221,230,105,0,166,105,0,166,105,0,166,105,105,105,105,105,105,0,15,16,0,241,0,15,17,16,241,167,31,3,189,112,105,5,
    209,105,62,32,106,213,0,111,171,0,106,12,96,105,3,226,105,0,6,144,0,105,0,6,144,0,105,0,6,237,210,111,80,14,198,218,5,220,106,225,
    169,198,154,110,92,105,93,164,198,144,229,76,110,32,151,109,160,151,105,196,151,106,77,151,106,9,215,106,1,231,6,222,128,78,33,198,137,0,122,137,
    0,122,77,17,198,7,222,128,110,237,54,144,123,105,7,182,237,211,105,0,6,144,0,6,222,128,78,33,198,137,0,122,137,0,106,77,44,198,7,221,
    225,0,0,67,110,221,54,144,123,105,8,166,238,226,105,30,54,144,107,25,219,54,176,87,46,147,0,22,200,120,6,178,173,180,157,254,208,13,32,0,
    210,0,13,32,0,210,0,13,32,105,0,180,105,0,180,105,0,180,106,0,180,77,18,226,8,238,112,167,0,167,76,1,226,14,37,192,9,121,96,4,
    189,16,0,219,0,166,9,176,76,106,13,208,136,45,28,163,180,13,121,119,225,9,197,60,192,5,241,14,128,93,18,226,10,139,96,1,219,0,2,221,
    0,11,106,128,123,1,227,138,1,227,29,56,144,5,205,32,0,200,0,0,166,0,0,166,0,125,222,176,1,211,0,152,0,76,0,29,48,7,237,218,
    62,115,176,59,3,176,59,3,176,62,96,178,7,96,58,0,208,10,48,103,2,176,126,48,179,11,48,179,11,48,179,126,48,8,96,42,177,99,84,204,
    201,18,1,192,44,200,2,50,240,76,191,26,97,241,76,174,16,120,0,7,189,193,124,26,119,144,121,124,10,119,189,193,27,217,7,160,129,150,0,7,
    160,130,27,217,0,0,8,113,205,183,138,28,121,96,151,138,12,113,205,183,27,217,7,128,196,156,187,71,144,81,27,218,0,8,177,225,191,162,208,45,
    2,208,45,0,28,219,120,160,199,150,9,119,178,215,26,186,114,97,197,24,183,0,120,0,7,189,176,123,12,71,128,165,120,10,87,128,165,103,0,120,
    120,120,120,120,6,112,0,7,128,120,7,128,120,7,128,135,42,32,120,0,7,131,209,121,212,7,253,16,121,168,7,130,227,120,120,120,120,120,120,122,
    216,173,55,161,227,121,120,14,21,167,128,225,90,120,14,21,160,122,219,7,176,196,120,10,87,128,165,120,10,80,27,234,7,160,197,150,9,119,160,197,
    27,218,0,123,220,23,193,167,121,7,151,192,167,123,220,23,128,0,69,0,0,28,219,120,161,199,150,9,120,160,199,28,219,112,0,135,0,5,64,123,
    224,124,0,120,0,120,0,120,0,43,199,7,161,64,43,231,3,50,240,60,200,0,0,5,176,206,117,176,91,4,176,28,112,120,10,87,128,165,120,10,
    86,160,213,28,203,80,166,10,102,160,225,30,75,0,187,96,6,241,0,181,78,10,103,135,226,210,59,168,109,0,219,60,144,10,144,229,0,122,46,16,
    218,112,8,241,1,217,112,136,30,32,166,10,102,160,225,30,75,0,187,96,6,241,0,107,0,91,32,0,109,238,0,10,112,5,192,1,211,0,127,221,
    16,0,0,5,210,9,96,11,64,124,16,10,80,9,96,4,194,8,30,30,30,30,30,30,30,30,30,123,0,12,48,10,80,4,241,10,80,12,48,123,
    0,28,163,114,68,196,78,4,224,78,3,224,20,3,208,61,121,45,120,21,51,8,103,96,126,238,224,12,28,16,13,13,0,207,239,144,74,58,0,0,
    80,1,174,179,125,135,115,236,64,2,189,152,168,125,42,252,64,5,0,27,177,10,32,57,147,103,0,8,130,160,0,0,10,38,129,0,103,27,117,2,
    176,10,178,9,214,0,30,60,0,13,213,0,78,213,192,153,79,160,59,235,209,61,45,21,2,48,167,31,35,240,93,3,240,30,48,136,50,7,160,47,
    16,243,13,80,243,62,24,128,21,147,28,246,40,183,1,48,0,81,0,12,48,78,254,160,12,48,0,98,0,76,104,50,110,227,3,60,2,208,105,10,
    80,225,60,7,128,180,0,9,236,37,225,155,154,4,233,160,78,93,25,176,158,194,61,232,126,4,224,78,4,224,78,27,234,23,161,199,0,29,64,27,
    144,28,144,8,255,249,26,235,35,96,183,1,237,16,1,185,122,26,178,174,179,0,127,80,2,237,80,10,124,80,77,12,80,159,255,241,0,12,80,47,
    255,99,208,0,94,221,49,48,154,89,26,145,174,194,8,235,37,210,118,139,187,57,210,155,109,24,176,174,195,191,255,80,2,242,0,153,0,47,32,9,
    160,1,227,0,26,235,53,208,168,11,237,22,194,169,139,9,178,173,179,27,235,24,177,169,139,27,193,188,171,73,27,113,174,161,3,60,0,3,60,45,
    0,48,0,3,144,105,5,64,0,1,48,40,231,79,130,2,158,146,0,24,128,63,255,160,0,0,63,255,160,19,0,2,219,64,0,93,160,108,197,58,
    64,0,60,215,7,100,241,1,186,0,107,0,2,48,0,137,0,1,140,201,32,12,130,21,209,90,61,204,120,134,163,60,58,150,149,92,88,107,59,170,
    211,13,146,19,0,1,140,218,0,1,237,0,5,222,64,10,155,144,14,103,224,95,238,244,171,0,185,127,252,71,176,154,126,206,55,176,139,123,7,231,
    255,214,7,222,128,79,67,230,139,0,17,139,0,17,79,83,230,7,222,128,127,252,64,123,23,225,123,0,213,123,0,213,123,22,225,127,252,64,127,255,
    103,176,0,127,255,55,176,0,123,0,7,255,246,127,255,87,176,0,123,0,7,255,241,123,0,7,176,0,6,222,128,79,67,229,139,0,16,154,11,233,
    94,66,214,7,222,144,123,0,183,123,0,183,127,255,247,123,0,183,123,0,183,123,0,183,123,123,123,123,123,123,0,15,32,0,242,0,15,34,16,242,
    169,63,19,189,112,123,6,226,123,62,64,123,216,0,127,189,16,123,12,128,123,3,243,123,0,7,176,0,123,0,7,176,0,123,0,7,255,243,127,128,
    31,231,221,6,222,123,227,186,231,186,143,94,123,94,180,231,176,230,78,127,32,168,126,176,168,123,213,168,123,77,168,123,10,232,123,2,232,7,222,128,
    79,67,214,139,0,155,139,0,139,78,84,215,7,222,128,127,253,71,176,156,123,9,199,255,212,123,0,7,176,0,7,222,128,79,67,214,139,0,155,139,
    27,139,78,90,230,7,222,226,0,0,51,127,253,64,123,9,192,123,9,176,127,255,64,123,30,64,123,8,192,25,235,55,209,120,62,164,0,39,217,138,
    24,210,174,180,191,255,241,0,228,0,0,228,0,0,228,0,0,228,0,0,228,0,123,0,213,123,0,213,123,0,213,123,0,213,94,53,243,9,238,112,
    170,0,185,94,1,244,14,85,224,9,153,144,4,221,64,0,238,0,169,9,208,94,6,192,222,40,176,47,46,165,199,0,232,183,158,48,10,215,61,224,
    0,111,48,234,0,94,34,243,10,171,128,1,237,16,2,238,16,12,138,176,125,17,230,140,1,230,30,87,192,6,222,64,0,202,0,0,168,0,0,168,
    0,143,255,208,1,213,0,170,0,94,16,30,64,8,255,253,79,132,192,76,4,192,76,4,192,79,128,180,7,128,60,0,225,10,80,105,2,208,143,64,
    196,12,64,196,12,64,196,143,64,9,112,42,193,100,85,238,235,18,1,193,44,217,2,67,242,76,207,42,130,242,93,175,32,122,0,7,205,210,125,43,
    135,160,138,125,43,151,205,210,27,233,8,193,162,168,0,8,193,162,27,233,0,0,10,130,205,200,140,45,138,144,168,140,29,130,205,200,27,234,8,161,
    212,173,187,88,176,97,27,234,0,9,208,31,32,223,208,47,0,47,0,47,0,47,0,44,219,136,194,216,169,10,136,194,216,27,203,130,97,199,26,200,
    16,122,0,7,189,193,125,45,103,160,183,122,11,119,160,183,105,17,122,122,122,122,122,6,144,17,8,160,138,8,160,138,8,160,153,60,48,122,0,7,
    163,227,123,214,7,255,32,123,170,7,162,228,122,122,122,122,122,122,123,218,158,87,194,246,139,122,14,54,199,160,227,108,122,14,54,192,123,220,23,210,
    214,122,11,119,160,183,122,11,112,27,234,8,193,214,168,10,136,193,214,27,234,16,124,221,39,210,184,122,8,167,209,185,124,221,39,160,0,70,0,0,
    44,219,136,194,216,169,10,136,193,216,44,220,128,0,168,0,5,64,124,226,125,32,122,0,122,0,122,0,60,216,8,177,80,44,249,4,67,242,60,217,
    0,0,5,208,223,149,208,93,5,208,28,160,122,11,119,160,183,122,11,118,193,215,28,219,112,183,11,134,192,227,31,93,0,188,128,7,243,0,183,79,
    26,136,167,228,212,77,169,143,16,237,61,176,11,176,232,0,123,46,33,235,144,9,243,1,234,160,154,30,48,183,10,134,192,227,31,93,0,204,128,7,
    243,0,109,0,92,64,0,126,255,0,11,144,5,209,1,229,0,143,238,32,4,197,9,144,27,112,125,16,11,112,9,144,4,213,24,1,240,31,1,240,
    31,1,240,31,1,240,31,1,240,139,16,12,64,11,128,3,243,11,112,12,64,139,16,29,164,163,101,211,63,51,243,47,34,242,27,16,97,46,32,31,
    95,17,243,240,11,27,0,4,176,224,6,146,208,127,255,250,11,71,128,207,255,244,30,12,48,60,14,16,0,51,0,7,221,112,95,136,244,79,148,16,
    7,238,128,34,72,246,126,136,246,24,238,144,0,51,0,10,196,1,193,2,180,144,164,0,10,196,74,0,0,0,28,16,0,0,8,83,203,16,3,176,
    118,164,0,194,3,203,16,6,218,0,14,76,64,13,174,32,11,246,51,125,141,183,156,44,243,43,236,201,31,33,241,11,0,5,224,11,144,15,80,47,
    48,63,32,31,64,13,112,6,208,0,48,139,3,242,14,96,200,11,144,215,47,71,192,34,0,3,195,1,190,177,28,220,32,8,0,0,103,0,1,137,
    16,63,255,244,0,137,0,0,103,0,47,21,192,86,0,1,16,111,249,6,2,226,0,179,2,241,6,176,10,112,14,48,62,0,122,0,182,0,6,222,
    112,63,101,243,125,0,216,140,0,185,125,0,200,63,101,244,6,222,112,43,244,152,244,17,244,1,244,1,244,1,244,1,244,9,237,80,110,56,224,18,
    5,224,0,29,128,1,218,0,29,194,16,127,255,243,9,237,96,95,71,241,0,7,224,0,223,96,0,5,243,126,70,244,26,238,128,0,47,208,0,174,
    208,3,231,208,11,118,208,94,39,209,143,255,250,0,7,208,15,255,240,47,50,32,63,190,112,23,38,242,18,1,244,79,71,242,8,237,96,6,222,128,
    63,101,178,125,174,144,143,85,244,126,0,230,63,86,243,6,238,112,175,255,209,34,187,0,47,64,9,192,1,245,0,141,0,30,96,0,8,238,112,79,
    70,242,47,54,225,9,255,96,94,36,243,126,36,245,26,238,128,9,237,64,95,72,225,140,1,244,110,55,246,27,233,245,74,56,225,9,237,64,45,32,
    96,0,0,96,45,32,5,1,243,0,0,0,43,20,208,104,0,0,0,0,0,4,179,6,221,113,79,161,0,5,206,129,0,4,163,1,17,16,47,255,
    243,1,17,16,47,255,243,0,0,0,44,80,0,7,221,113,0,9,246,7,221,97,43,64,0,42,234,40,179,216,0,46,96,30,144,3,192,0,21,0,
    5,208,0,0,124,237,145,0,172,65,40,208,62,26,227,11,103,166,194,168,106,136,136,7,133,167,166,194,185,151,78,42,215,202,16,172,49,18,0,0,
    124,238,128,0,0,191,80,0,31,202,0,5,229,241,0,170,31,80,30,255,250,5,242,22,225,172,0,47,80,111,254,160,110,37,244,110,4,242,111,239,
    144,110,1,230,110,19,232,111,255,177,4,206,195,2,233,75,224,126,0,22,24,192,0,0,126,0,22,18,233,74,225,4,206,195,0,111,253,145,6,226,
    93,144,110,0,110,6,224,4,241,110,0,110,6,226,77,144,111,253,145,0,111,255,246,226,34,110,17,22,255,251,110,0,6,226,34,111,255,240,111,255,
    214,226,34,110,0,6,255,249,110,34,22,224,0,110,0,0,4,206,195,2,249,74,225,126,16,2,8,192,111,245,110,16,63,49,234,75,208,4,206,195,
    0,110,0,63,38,224,3,242,110,34,95,38,255,255,242,110,0,63,38,224,3,242,110,0,63,32,110,110,110,110,110,110,110,0,10,160,0,170,0,10,
    160,0,170,85,10,169,211,216,44,236,32,110,1,203,6,224,173,16,110,110,48,6,238,225,0,111,109,160,6,224,79,64,110,0,173,16,110,0,6,224,
    0,110,0,6,224,0,110,0,6,226,33,111,255,176,111,192,7,252,110,242,12,236,109,199,47,172,110,140,109,140,110,63,168,140,110,13,244,140,110,8,
    224,140,111,80,47,70,254,18,244,110,200,47,70,228,244,244,110,11,175,70,224,46,244,110,0,143,64,4,206,195,2,233,74,225,126,0,31,88,192,0,
    215,126,0,31,82,233,74,225,4,206,195,0,111,254,128,110,38,244,110,0,247,110,22,244,111,254,144,110,0,0,110,0,0,4,206,195,2,233,74,225,
    126,0,31,88,192,0,215,126,9,111,82,233,111,225,4,206,219,0,0,0,114,111,254,144,110,37,245,110,3,245,111,255,177,110,29,144,110,6,241,110,
    1,231,8,238,128,95,85,244,79,97,0,8,239,144,34,5,246,126,68,246,26,238,144,175,255,250,18,187,33,0,170,0,0,170,0,0,170,0,0,170,
    0,0,170,0,110,0,95,110,0,95,110,0,95,110,0,95,110,0,110,63,132,219,6,222,178,173,0,47,85,242,7,241,14,112,186,0,172,31,96,5,
    245,241,0,30,203,0,0,175,96,0,172,2,247,7,224,111,6,251,10,176,47,57,190,14,112,14,125,111,79,48,10,175,28,158,0,6,237,8,235,0,
    2,249,4,247,0,79,64,157,16,173,63,64,1,238,160,0,9,244,0,3,252,192,0,203,47,112,126,32,126,32,142,16,127,33,232,30,128,6,231,225,
    0,13,247,0,0,111,0,0,5,240,0,0,95,0,0,143,255,247,18,40,225,0,46,96,0,187,0,6,226,0,46,114,33,127,255,247,63,227,241,63,
    19,241,63,19,241,63,19,254,0,0,165,0,137,0,77,0,14,32,11,96,7,176,3,224,0,228,143,144,169,10,144,169,10,144,169,10,152,249,0,0,
    9,225,1,216,112,102,43,0,255,255,64,0,0,29,32,50,27,235,51,97,201,59,205,169,161,202,77,219,160,125,0,0,125,0,0,125,174,112,127,71,
    242,126,2,244,127,71,242,125,190,112,9,236,55,227,136,155,0,7,227,120,10,236,48,0,3,242,0,3,242,27,233,242,126,57,242,156,4,242,126,57,
    242,27,233,242,10,236,48,125,40,192,158,204,192,125,36,80,26,236,64,7,229,14,96,239,244,31,64,31,64,31,64,31,64,27,233,242,126,57,242,156,
    4,242,126,40,242,27,233,242,38,22,240,26,235,80,125,0,0,125,0,0,125,173,96,127,73,208,125,4,240,125,4,240,125,4,240,91,19,125,125,125,
    125,125,5,176,19,7,208,125,7,208,125,7,208,140,61,80,125,0,0,125,0,0,125,28,160,125,172,0,127,247,0,126,111,48,125,11,192,125,125,125,
    125,125,125,125,124,173,76,195,127,75,211,216,125,8,176,170,125,8,176,170,125,8,176,170,124,173,96,127,73,208,125,4,240,125,4,240,125,4,240,9,
    237,64,126,57,224,155,4,242,126,57,224,10,237,64,125,174,112,127,71,242,126,2,244,127,71,242,125,174,112,125,0,0,90,0,0,27,233,242,126,57,
    242,156,4,242,126,57,242,27,234,242,0,3,242,0,2,177,125,183,127,81,125,0,125,0,125,0,43,235,23,225,83,43,252,52,113,201,43,235,48,18,
    0,79,16,239,240,79,16,79,16,63,48,11,225,125,4,240,125,4,240,125,4,240,95,73,240,11,232,240,170,4,241,94,25,176,14,94,80,9,190,16,
    4,249,0,185,14,128,245,125,46,196,241,47,121,231,192,13,213,189,112,9,241,127,48,110,27,160,203,225,6,249,0,217,226,141,10,176,170,4,241,94,
    25,176,14,94,80,9,190,16,4,249,0,3,244,0,94,128,0,111,255,128,23,226,3,229,1,217,17,127,255,144,1,186,5,225,6,192,27,176,110,48,
    8,192,6,209,1,187,179,244,244,244,244,244,244,244,244,244,244,139,48,28,128,9,144,7,210,1,217,8,177,11,144,140,48,10,211,148,61,78,209,0,
    0,0,31,65,244,31,64,243,15,48,97,7,17,211,244,243,227,242,210,210,1,224,119,0,60,10,80,111,255,255,48,135,14,16,9,81,224,12,255,255,
    192,14,22,144,1,224,135,0,0,8,0,0,109,251,32,47,122,203,4,242,129,48,28,236,64,0,5,223,144,53,8,111,5,246,171,208,8,239,195,0,
    0,128,0,7,215,0,74,0,13,13,1,193,0,13,45,8,96,0,5,181,59,0,0,0,0,195,0,0,0,6,128,124,128,0,44,16,224,224,0,164,
    0,124,128,5,237,48,0,214,122,0,12,106,144,0,111,193,0,29,220,27,23,193,204,224,126,38,249,1,174,217,227,243,227,210,1,242,7,192,12,128,
    14,80,31,48,15,64,13,112,8,176,2,242,0,16,123,2,242,12,112,170,8,192,155,11,129,244,108,1,16,2,178,17,205,197,26,236,64,59,34,0,
    16,0,0,61,0,0,62,0,31,255,252,0,62,0,0,61,0,31,51,208,89,0,95,254,1,17,7,17,227,0,101,0,196,1,241,5,176,10,112,14,
    48,62,0,122,0,182,0,3,206,161,1,232,61,128,95,16,110,7,208,4,241,125,0,79,21,224,6,224,30,131,216,0,76,234,16,25,247,138,215,48,
    199,0,199,0,199,0,199,0,199,0,199,7,238,144,63,84,245,38,0,198,0,3,242,0,45,112,1,218,0,28,194,17,95,255,249,6,222,161,63,83,
    215,0,2,214,0,159,160,0,3,216,37,0,156,79,100,218,7,238,161,0,10,243,0,5,223,48,1,213,243,0,139,15,48,62,32,243,7,255,255,243,
    17,17,244,0,0,15,48,13,255,246,14,65,17,15,32,0,47,190,177,22,66,216,2,0,155,63,83,216,6,222,145,2,206,161,13,148,216,78,0,52,
    108,158,194,127,114,202,95,0,125,30,115,201,4,222,161,159,255,245,17,20,243,0,9,176,0,47,64,0,140,0,1,230,0,7,224,0,13,112,0,6,
    222,161,47,98,216,30,81,214,5,255,176,47,82,216,110,0,140,95,82,202,7,238,178,7,222,128,79,100,230,125,0,155,79,67,221,9,236,156,36,0,
    154,63,101,228,7,237,112,3,1,244,3,0,0,7,17,227,0,0,229,5,16,0,0,1,227,62,5,144,0,0,19,0,23,233,24,234,64,47,179,0,
    1,142,180,0,1,121,255,255,177,17,17,17,17,31,255,251,64,0,13,197,0,6,205,96,5,220,109,214,11,80,0,27,237,71,210,172,17,8,192,6,
    245,0,245,0,24,16,1,113,0,62,32,0,75,238,197,0,7,213,17,59,128,46,38,236,194,211,91,30,52,241,166,121,60,0,225,150,91,30,53,243,
    195,46,38,219,124,128,7,213,16,33,0,0,91,238,196,0,0,111,144,0,10,206,0,1,245,244,0,95,28,144,10,177,141,0,239,255,243,79,32,13,
    137,192,0,141,79,255,195,4,242,43,192,79,0,169,4,254,254,48,79,1,173,4,240,4,241,79,34,174,4,255,253,80,1,174,216,0,204,70,231,79,
    32,6,134,208,0,0,109,0,0,4,242,0,104,12,180,110,112,26,237,128,79,254,179,4,242,57,225,79,0,13,116,240,0,169,79,0,10,148,240,0,
    215,79,35,158,20,255,235,48,79,255,247,79,34,33,79,0,0,79,255,243,79,33,16,79,0,0,79,33,17,79,255,247,79,255,246,79,34,33,79,0,
    0,79,33,16,79,255,241,79,0,0,79,0,0,79,0,0,1,174,215,0,204,70,247,79,32,6,134,208,0,0,125,0,255,228,241,0,141,12,180,110,
    128,42,237,128,79,0,9,180,240,0,155,79,0,9,180,255,255,251,79,34,42,180,240,0,155,79,0,9,180,240,0,155,79,4,240,79,4,240,79,4,
    240,79,4,240,0,3,241,0,3,241,0,3,241,0,3,241,0,3,241,120,4,241,126,57,224,26,237,64,79,0,95,84,240,46,128,79,29,160,4,249,
    242,0,79,221,144,4,243,95,48,79,0,172,4,240,2,231,79,0,0,79,0,0,79,0,0,79,0,0,79,0,0,79,0,0,79,33,16,79,255,243,
    79,208,0,175,116,254,48,30,231,79,184,5,220,116,246,208,169,199,79,47,62,92,116,240,201,224,199,79,7,250,12,116,240,47,80,199,79,112,7,212,
    254,16,125,79,169,7,212,243,243,125,79,25,183,212,241,30,173,79,16,127,212,241,0,221,1,174,215,0,12,196,94,112,79,32,7,224,109,0,3,241,
    109,0,3,241,79,32,6,224,12,180,94,112,1,174,215,0,79,255,195,79,35,204,79,0,111,79,2,188,79,255,211,79,33,0,79,0,0,79,0,0,
    1,174,215,0,12,196,94,112,79,32,7,224,109,0,3,241,109,0,3,241,79,35,134,224,12,180,223,112,1,174,222,64,0,0,4,96,79,255,195,4,
    242,60,192,79,0,111,4,242,59,192,79,255,227,4,240,110,16,79,0,216,4,240,6,225,6,222,178,2,247,59,176,79,48,19,1,206,163,0,0,90,
    248,3,64,6,224,95,99,189,0,141,236,48,159,255,255,49,38,226,32,0,94,0,0,5,224,0,0,94,0,0,5,224,0,0,94,0,0,5,224,0,
    79,0,10,148,240,0,169,79,0,10,148,240,0,169,79,0,10,147,243,0,201,30,180,143,64,60,237,96,156,0,8,212,242,0,216,14,96,47,48,171,
    7,224,5,241,185,0,14,94,64,0,172,224,0,5,250,0,171,0,189,0,156,110,0,239,32,201,47,51,236,80,245,14,102,185,148,241,10,153,133,199,
    208,6,204,66,250,144,3,238,16,222,80,0,236,0,175,32,63,64,30,96,157,9,192,1,232,243,0,6,249,0,0,143,176,0,63,111,96,12,160,142,
    22,226,1,217,126,16,13,145,232,6,226,6,225,216,0,13,206,16,0,95,112,0,1,243,0,0,31,48,0,1,243,0,127,255,254,18,34,216,0,8,
    209,0,63,64,0,186,0,6,226,0,30,113,17,111,255,254,31,243,31,48,31,48,31,48,31,48,31,48,31,48,31,48,31,243,132,0,137,0,77,0,
    14,32,11,96,7,160,3,224,0,227,0,167,127,176,123,7,176,123,7,176,123,7,176,123,127,176,6,243,0,215,160,90,13,33,16,17,255,255,160,0,
    0,23,16,151,9,236,80,57,22,224,3,140,241,110,118,241,155,8,241,60,233,241,94,0,0,94,0,0,94,142,177,95,98,232,95,16,155,95,16,155,
    95,98,216,94,142,177,7,221,112,79,67,226,141,0,0,140,0,0,79,67,226,7,237,112,0,0,184,0,0,184,9,234,184,79,67,248,141,0,200,141,
    0,200,79,67,248,9,235,184,7,238,112,78,51,228,142,221,247,124,0,0,63,50,194,6,221,128,0,0,6,233,13,112,239,248,14,80,14,80,14,80,
    14,80,14,80,8,235,185,79,67,249,141,0,201,140,0,201,95,51,249,9,235,185,22,17,215,9,221,145,94,0,0,94,0,0,94,142,176,95,116,245,
    94,0,215,94,0,199,94,0,199,94,0,199,77,1,64,94,5,224,94,5,224,94,5,224,4,208,1,64,5,224,5,224,5,224,5,224,5,224,5,224,
    7,208,62,96,0,0,94,0,0,94,0,0,94,6,226,94,79,64,95,233,0,95,190,16,94,29,144,94,4,244,94,94,94,94,94,94,94,94,93,142,
    104,233,5,245,127,117,243,94,2,242,14,85,224,47,32,229,94,2,242,14,85,224,47,32,229,93,142,176,95,116,245,94,0,215,94,0,199,94,0,199,
    94,0,199,7,222,128,79,67,245,141,0,184,141,0,185,79,67,245,7,238,128,94,142,177,95,98,232,95,16,155,95,16,155,95,98,216,94,142,177,94,
    0,0,93,0,0,9,234,184,79,67,248,141,0,200,141,0,200,79,67,248,9,235,184,0,0,184,0,0,168,93,155,95,130,94,0,94,0,94,0,94,
    0,25,220,64,94,21,112,47,182,16,2,141,192,74,22,240,10,236,80,20,16,47,32,239,243,47,32,47,32,47,32,31,64,9,227,94,0,199,94,0,
    199,94,0,199,94,0,215,63,85,247,10,233,199,170,0,199,94,18,242,14,70,192,9,155,112,4,222,32,0,236,0,170,8,224,79,22,208,191,40,176,
    47,46,165,183,0,215,182,142,48,9,216,45,224,0,95,64,218,0,94,21,226,12,140,112,3,253,0,4,237,16,13,108,128,125,4,242,170,0,200,78,
    18,242,14,87,192,9,156,112,4,223,32,0,219,0,2,230,0,79,160,0,0,0,0,95,255,224,1,29,128,0,141,16,4,243,0,29,145,16,111,255,
    241,0,141,2,244,4,224,7,208,95,64,25,192,4,224,3,242,0,174,197,198,198,198,198,198,198,198,198,198,198,198,123,64,10,160,6,192,5,226,0,
    173,4,228,6,192,9,160,124,64,10,231,58,45,59,247,0,0,0,238,13,224,222,13,208,205,12,208,17,11,176,188,0,205,79,107,195,245,187,47,70,
    113,163,0,170,9,176,0,215,12,128,79,255,255,248,39,246,127,114,5,240,79,16,57,229,143,81,207,255,255,240,11,128,185,0,14,96,215,0,0,0,
    0,0,0,6,80,0,2,190,234,32,29,252,207,208,63,166,86,146,31,251,112,0,4,207,252,80,0,6,174,242,92,86,89,245,47,236,206,225,4,206,
    235,64,0,6,80,0,0,0,0,0,78,212,0,13,80,12,120,176,8,160,0,198,124,3,225,0,5,238,80,213,0,0,0,0,138,0,0,0,0,62,
    19,223,112,0,13,80,154,78,0,8,160,9,148,224,3,225,0,62,247,0,0,0,0,0,0,1,190,194,0,9,248,219,0,10,208,187,0,6,252,244,
    0,4,255,112,82,47,188,228,246,127,65,223,242,79,199,207,208,6,222,199,249,205,188,187,103,11,224,63,144,143,64,191,16,222,0,252,0,237,0,191,
    0,143,64,47,144,10,208,127,64,47,176,11,241,8,243,6,245,5,247,6,246,8,244,11,241,47,160,110,48,1,61,1,12,173,200,3,239,177,12,157,
    184,1,60,0,0,52,0,0,10,192,0,0,172,0,14,255,255,241,102,205,102,0,10,192,0,0,121,0,0,8,128,233,31,84,241,22,102,83,255,252,
    187,187,0,47,80,7,241,0,188,0,14,128,4,244,0,143,16,12,176,1,247,0,95,48,9,224,0,85,0,0,0,157,233,16,10,250,175,176,47,176,
    10,243,95,112,6,246,111,96,5,247,95,112,6,246,47,176,10,243,10,250,159,176,1,157,233,16,5,239,102,253,246,101,111,96,6,246,0,111,96,6,
    246,0,111,96,6,246,0,111,96,3,190,216,1,238,140,247,60,96,31,176,0,3,249,0,1,222,32,2,222,48,2,222,48,2,223,167,118,79,255,255,
    208,2,190,233,16,13,232,175,144,24,80,14,192,0,21,159,96,0,47,250,32,0,0,78,208,57,80,12,242,46,233,175,208,4,190,234,32,0,2,239,
    112,0,10,255,112,0,79,159,112,0,220,63,112,7,244,63,112,47,160,63,112,111,255,255,248,55,119,159,180,0,0,63,112,10,255,255,144,12,215,119,
    80,13,176,0,0,14,204,235,32,11,198,159,176,0,0,12,241,41,96,12,240,13,232,175,160,2,190,217,16,0,141,234,16,9,251,175,176,47,160,5,
    112,95,123,236,48,111,232,143,208,95,160,10,242,63,160,11,242,11,249,159,176,1,158,234,16,143,255,255,132,119,123,247,0,0,206,16,0,79,128,0,
    12,242,0,4,249,0,0,191,32,0,79,144,0,11,242,0,0,2,190,233,16,13,248,159,160,31,176,14,208,9,231,143,96,3,223,251,32,63,194,62,
    225,111,128,11,243,47,231,143,225,4,190,234,32,3,190,215,0,30,233,191,112,95,112,13,224,111,112,13,242,47,231,175,243,5,222,153,241,39,64,13,
    208,30,233,207,96,3,190,214,0,187,187,0,0,187,187,9,192,9,192,0,0,0,0,1,16,13,192,15,128,47,48,40,0,0,0,2,128,0,58,254,
    5,207,232,32,255,96,0,9,239,164,0,1,126,252,0,0,5,176,103,119,119,223,255,255,0,0,0,223,255,255,103,119,119,115,0,0,12,251,64,0,
    23,223,198,0,0,94,243,3,158,250,26,254,130,0,166,16,0,0,7,222,179,4,252,143,224,72,32,207,16,0,95,176,0,95,177,0,10,224,0,0,
    35,0,0,10,192,0,0,172,0,0,0,0,19,66,0,0,0,60,255,255,230,0,4,252,66,18,126,128,13,192,142,220,165,243,63,69,248,143,160,231,
    95,41,176,10,160,201,111,42,176,11,160,216,63,70,246,127,196,244,30,176,158,213,206,128,5,251,65,2,48,0,0,76,255,255,224,0,0,0,36,83,
    0,0,0,13,252,0,0,3,254,242,0,0,143,127,112,0,13,225,252,0,3,250,12,242,0,143,166,191,112,13,255,255,252,3,251,0,13,242,143,96,
    0,127,112,63,255,236,64,63,199,159,224,63,144,12,241,63,163,111,144,63,255,253,80,63,144,44,243,63,144,7,246,63,199,158,243,63,255,253,96,0,
    92,237,129,0,127,234,223,160,30,209,0,207,36,248,0,1,33,111,96,0,0,4,247,0,1,33,31,209,0,207,32,127,218,223,160,0,108,237,112,0,
    63,255,218,32,3,252,138,255,48,63,144,3,251,3,249,0,12,224,63,144,0,191,19,249,0,12,224,63,144,3,251,3,252,138,255,48,63,255,218,32,
    0,63,255,255,147,252,119,117,63,144,0,3,252,119,114,63,255,255,83,249,0,0,63,144,0,3,252,119,116,63,255,255,160,63,255,255,131,252,119,116,
    63,144,0,3,249,0,0,63,255,255,35,252,119,113,63,144,0,3,249,0,0,63,144,0,0,0,92,237,129,0,127,218,223,176,47,209,0,173,37,247,
    0,0,0,111,96,63,255,100,248,1,89,245,30,225,0,175,48,127,234,207,160,0,92,237,129,0,63,144,0,159,51,249,0,9,243,63,144,0,159,51,
    252,119,124,243,63,255,255,255,51,249,0,9,243,63,144,0,159,51,249,0,9,243,63,144,0,159,48,63,147,249,63,147,249,63,147,249,63,147,249,63,
    144,0,0,143,48,0,8,243,0,0,143,48,0,8,243,0,0,143,51,65,8,243,143,64,175,37,252,142,208,7,222,195,0,63,144,6,252,3,249,3,
    253,16,63,146,238,48,3,248,191,80,0,63,239,245,0,3,255,126,225,0,63,160,111,160,3,249,0,191,80,63,144,2,254,16,63,144,0,3,249,0,
    0,63,144,0,3,249,0,0,63,144,0,3,249,0,0,63,144,0,3,252,119,114,63,255,255,80,63,250,0,8,255,99,255,241,0,223,246,63,207,96,
    63,207,99,249,235,8,248,246,63,139,241,221,111,99,248,111,111,134,246,63,129,253,243,111,99,248,10,253,6,246,63,128,95,128,111,96,63,226,0,127,
    83,255,160,7,245,63,223,64,127,83,249,221,7,245,63,149,247,127,83,249,12,232,245,63,144,63,223,83,249,0,159,245,63,144,1,239,80,0,92,236,
    112,0,127,218,207,160,30,209,0,191,68,248,0,4,248,111,96,0,47,164,247,0,4,248,30,209,0,175,64,127,217,207,176,0,92,237,128,0,63,255,
    235,32,63,199,191,208,63,144,11,243,63,144,11,243,63,183,175,209,63,255,235,48,63,144,0,0,63,144,0,0,63,144,0,0,0,92,236,112,0,127,
    218,207,160,30,209,0,191,68,248,0,4,248,111,96,0,47,164,247,5,84,248,30,209,62,191,64,127,218,255,160,0,92,237,234,0,0,0,3,147,63,
    255,235,48,63,199,191,209,63,144,11,243,63,144,10,243,63,183,175,224,63,255,254,32,63,144,191,48,63,144,63,192,63,144,10,245,2,174,234,32,29,
    250,175,208,63,160,6,145,30,248,48,0,4,207,252,80,0,2,126,242,92,96,9,245,46,233,175,225,4,190,235,48,143,255,255,248,71,126,231,116,0,
    13,208,0,0,13,208,0,0,13,208,0,0,13,208,0,0,13,208,0,0,13,208,0,0,13,208,0,63,144,0,207,63,144,0,207,63,144,0,207,63,
    144,0,207,63,144,0,207,63,144,0,207,31,208,2,253,10,252,157,247,1,141,236,96,143,96,0,127,131,251,0,12,243,13,241,1,253,0,143,96,111,
    128,3,250,10,243,0,13,224,237,0,0,143,127,128,0,3,254,243,0,0,12,253,0,0,159,80,12,244,0,239,21,249,1,255,112,47,176,31,192,95,
    219,6,248,0,207,24,249,224,159,64,8,244,204,95,60,224,0,79,126,130,246,251,0,1,251,244,13,207,112,0,12,255,16,159,243,0,0,143,192,5,
    254,0,0,46,209,2,238,16,127,144,175,80,0,207,95,160,0,3,255,225,0,0,11,250,0,0,5,254,244,0,1,238,63,209,0,175,80,143,128,95,
    160,1,223,48,111,144,0,223,48,223,32,111,144,4,250,13,226,0,11,248,247,0,0,47,253,16,0,0,159,96,0,0,7,244,0,0,0,127,64,0,
    0,7,244,0,0,95,255,255,244,39,119,143,225,0,0,159,80,0,5,250,0,0,30,209,0,0,175,64,0,5,249,0,0,30,247,119,114,95,255,255,
    245,85,94,254,235,14,176,235,14,176,235,14,176,235,14,176,239,229,85,188,0,7,241,0,63,80,0,233,0,10,208,0,95,32,1,247,0,12,176,0,
    142,0,4,244,0,23,48,37,82,111,247,4,247,4,247,4,247,4,247,4,247,4,247,4,247,4,247,111,247,37,82,0,34,0,2,254,16,9,172,112,
    47,54,208,71,0,131,255,255,251,85,85,84,188,2,194,6,190,196,3,251,109,225,0,19,207,49,175,238,243,143,96,159,56,248,110,243,26,235,143,48,
    79,112,0,0,79,112,0,0,79,138,234,16,79,232,175,144,79,160,14,224,79,128,12,241,79,160,14,224,79,248,175,144,79,139,234,16,3,190,214,1,
    238,124,244,111,112,39,39,245,0,0,111,112,39,49,238,123,244,3,190,214,0,0,0,14,192,0,0,236,5,221,110,194,254,123,252,111,112,47,199,245,
    0,252,111,128,47,194,254,123,252,5,221,110,192,3,190,213,1,237,124,243,111,113,79,135,255,255,250,111,96,0,1,237,105,245,3,190,199,0,0,54,
    48,111,247,11,242,13,255,246,92,245,32,191,0,11,240,0,191,0,11,240,0,191,0,5,221,110,193,238,123,252,111,128,47,199,245,0,236,111,112,47,
    194,253,106,252,5,221,110,193,101,20,251,10,255,253,48,1,51,0,79,112,0,4,247,0,0,79,138,235,20,254,156,248,79,160,47,164,247,1,251,79,
    112,31,180,247,1,251,79,112,31,176,26,66,229,0,4,247,79,116,247,79,116,247,79,116,247,1,164,2,229,0,0,4,247,4,247,4,247,4,247,4,
    247,4,247,4,247,6,247,79,226,37,16,79,112,0,4,247,0,0,79,112,175,100,247,127,144,79,191,176,4,255,249,0,79,188,244,4,247,47,209,79,
    112,127,144,79,116,247,79,116,247,79,116,247,79,116,247,79,112,79,123,229,76,230,4,253,158,236,158,241,79,144,127,96,175,52,247,6,245,8,243,79,
    112,111,80,143,52,247,6,245,8,243,79,112,111,80,143,48,79,122,235,20,254,156,248,79,160,47,164,247,1,251,79,112,31,180,247,1,251,79,112,31,
    176,3,190,214,1,238,123,245,111,112,47,183,245,0,237,111,112,47,177,238,123,245,3,190,214,0,79,138,234,16,79,232,175,144,79,160,14,224,79,128,
    12,241,79,160,14,224,79,232,175,144,79,155,234,16,79,112,0,0,79,112,0,0,21,48,0,0,5,221,110,194,254,123,252,111,112,47,199,245,0,252,
    111,128,47,194,254,123,252,5,221,110,192,0,0,236,0,0,14,192,0,0,84,79,140,164,254,182,79,160,4,248,0,79,112,4,247,0,79,112,0,7,
    222,180,3,251,92,208,63,180,16,0,142,254,128,2,20,207,37,250,92,242,8,223,197,0,8,96,0,251,0,223,255,5,253,80,15,176,0,251,0,15,
    192,0,238,96,5,206,16,79,112,31,180,247,1,251,79,112,31,180,247,1,251,63,144,63,177,254,156,251,6,237,94,176,175,48,14,197,247,4,247,14,
    192,159,32,159,29,192,4,247,247,0,14,239,32,0,159,176,0,175,32,223,16,237,111,81,255,67,249,47,148,253,118,245,13,200,202,169,241,9,235,150,
    220,176,5,255,66,255,112,1,255,16,223,48,95,128,143,80,190,46,176,3,253,242,0,12,251,0,4,252,243,0,221,30,192,127,96,127,96,175,48,14,
    212,248,5,247,14,192,159,32,159,46,176,3,249,246,0,13,255,16,0,127,160,0,25,244,0,31,250,0,1,83,0,0,63,255,255,17,102,143,192,0,
    12,226,0,8,246,0,4,251,0,1,239,134,97,79,255,255,32,0,2,64,0,111,240,0,206,32,0,219,0,0,235,0,44,246,0,63,194,0,3,250,
    0,0,219,0,0,206,32,0,111,240,0,2,64,53,9,241,159,25,241,159,25,241,159,25,241,159,25,241,159,25,241,159,25,241,159,16,36,0,6,253,
    16,6,246,0,47,112,1,249,0,11,232,0,94,192,31,177,2,247,0,111,96,111,226,2,65,0,6,235,1,113,14,204,203,241,7,32,158,128,0,63,
    245,0,0,142,218,0,0,219,158,16,3,247,95,80,8,246,95,176,13,255,255,241,79,128,5,246,159,48,1,251,79,246,0,63,248,79,251,0,142,248,
    79,191,32,219,248,79,127,115,247,248,79,92,183,243,248,79,87,235,162,248,79,82,255,82,248,79,80,190,18,248,79,255,216,4,249,107,246,79,80,31,
    164,245,23,248,79,255,252,20,249,83,0,79,80,0,4,245,0,0,0,4,190,253,163,0,0,7,255,255,255,229,0,7,255,254,207,255,244,1,239,250,
    0,28,255,192,127,254,16,0,63,255,74,255,160,0,0,207,247,207,247,0,0,10,255,158,255,96,0,0,143,251,239,246,0,0,8,255,188,255,112,0,
    0,175,249,175,250,0,0,12,255,119,255,225,0,2,255,244,30,255,160,1,207,252,0,143,255,236,239,255,80,0,143,255,255,255,96,0,0,75,239,218,
    48,0,0,4,239,255,0,25,255,255,240,13,255,255,255,0,255,181,255,240,14,96,63,255,0,32,3,255,240,0,0,63,255,0,0,3,255,240,0,0,
    63,255,0,0,3,255,240,0,0,63,255,0,0,3,255,240,0,0,63,255,0,0,3,255,240,0,0,63,255,0,0,3,255,240,0,25,206,219,96,0,
    5,239,255,255,253,32,46,255,234,191,255,192,143,252,16,4,255,243,191,246,0,0,207,246,17,16,0,0,223,245,0,0,0,4,255,225,0,0,0,46,
    255,112,0,0,3,239,250,0,0,0,78,255,160,0,0,4,239,250,0,0,0,95,255,144,0,0,7,255,249,0,0,0,127,255,250,170,170,167,175,255,
    255,255,255,250,175,255,255,255,255,250,0,23,190,236,129,0,0,62,255,255,255,228,0,29,255,235,174,255,226,5,255,210,0,29,255,96,72,133,0,0,
    175,248,0,0,0,0,29,255,80,0,0,87,141,255,160,0,0,11,255,254,80,0,0,0,191,255,255,145,0,0,0,1,110,255,144,0,0,0,0,111,
    255,24,153,64,0,5,255,242,175,253,32,1,207,254,4,255,255,203,239,255,112,6,255,255,255,255,144,0,2,140,238,201,48,0,0,0,0,143,255,208,
    0,0,0,63,255,253,0,0,0,12,255,255,208,0,0,7,255,239,253,0,0,2,255,229,255,208,0,0,191,247,63,253,0,0,111,252,3,255,208,0,
    46,255,48,63,253,0,10,255,128,3,255,208,5,255,208,0,63,253,0,223,252,170,171,255,234,142,255,255,255,255,255,253,239,255,255,255,255,255,208,0,
    0,0,79,253,0,0,0,0,4,255,208,0,0,0,0,79,253,0,8,255,255,255,255,242,0,175,255,255,255,255,32,11,255,203,187,187,178,0,223,242,
    0,0,0,0,14,255,0,0,0,0,0,255,224,37,82,0,0,47,253,159,255,251,32,3,255,255,255,255,253,16,42,206,114,22,239,248,0,0,0,0,
    7,255,192,0,0,0,0,63,255,5,136,80,0,4,255,208,95,254,32,1,207,250,1,223,254,170,239,254,32,3,239,255,255,254,80,0,1,123,238,200,
    16,0,0,3,173,236,146,0,0,7,255,255,255,247,0,5,255,255,239,255,244,0,223,250,16,44,255,176,95,253,0,0,57,153,9,255,128,20,82,0,
    0,191,245,126,255,253,48,13,255,175,255,255,255,48,255,255,179,20,223,252,14,255,209,0,4,255,241,207,249,0,0,14,255,57,255,176,0,1,255,241,
    63,255,80,0,175,253,0,175,255,186,223,255,80,1,175,255,255,255,112,0,0,90,222,201,32,0,63,255,255,255,255,255,63,255,255,255,255,255,43,187,
    187,187,223,255,0,0,0,0,207,249,0,0,0,5,255,242,0,0,0,12,255,144,0,0,0,95,255,32,0,0,0,207,249,0,0,0,5,255,242,0,
    0,0,12,255,144,0,0,0,95,255,32,0,0,0,207,248,0,0,0,5,255,225,0,0,0,13,255,128,0,0,0,95,254,16,0,0,0,223,248,0,
    0,0,0,23,190,236,146,0,0,46,255,255,255,246,0,13,255,233,157,255,243,4,255,242,0,12,255,128,95,253,0,0,143,250,2,255,242,0,12,255,
    96,7,255,232,124,255,176,0,4,223,255,255,112,0,5,239,255,255,255,128,4,255,247,33,94,255,144,207,249,0,0,95,255,30,255,112,0,3,255,244,
    191,252,0,0,143,255,21,255,253,135,191,255,160,7,255,255,255,255,176,0,2,140,238,202,64,0,0,24,190,235,112,0,0,78,255,255,255,194,0,46,
    255,235,191,255,209,9,255,210,0,46,255,112,207,246,0,0,127,253,14,255,64,0,5,255,241,207,248,0,0,159,255,56,255,246,18,143,255,244,29,255,
    255,255,191,255,32,43,255,255,162,255,240,0,2,85,32,79,253,7,153,80,0,10,255,144,127,255,64,7,255,242,2,239,255,238,255,249,0,3,239,255,
    255,251,16,0,1,140,238,182,0,0,4,151,1,239,245,47,255,112,142,177,0,0,0,0,0,0,0,0,0,0,4,151,1,239,245,47,255,112,142,177,
};
static const PGlyph kPGlyphs[584] = {
    {0,0,0,0,0,11}, {0,2,5,0,-5,16}, {5,3,2,0,-5,26}, {8,4,5,0,-5,31}, {18,4,7,0,-6,31}, {32,6,5,0,-5,49},
    {47,4,5,0,-5,32}, {57,2,2,0,-5,16}, {59,3,6,0,-5,18}, {68,2,6,0,-5,18}, {74,3,3,0,-5,27}, {79,4,4,0,-4,33},
    {87,2,2,0,-1,16}, {89,3,1,0,-2,22}, {91,2,1,0,-1,16}, {92,3,6,0,-5,19}, {101,4,5,0,-5,32}, {111,3,5,0,-5,21},
    {119,4,5,0,-5,30}, {129,4,5,0,-5,31}, {139,4,5,0,-5,32}, {149,4,5,0,-5,31}, {159,4,5,0,-5,31}, {169,4,5,0,-5,28},
    {179,4,5,0,-5,31}, {189,4,5,0,-5,31}, {199,2,3,0,-3,16}, {202,2,5,0,-3,16}, {207,4,4,0,-4,33}, {215,4,3,0,-3,33},
    {221,4,4,0,-4,33}, {229,4,5,0,-5,27}, {239,6,7,0,-5,49}, {260,5,5,0,-5,36}, {273,4,5,0,-5,32}, {283,5,5,0,-5,36},
    {296,5,5,0,-5,35}, {309,4,5,0,-5,29}, {319,4,5,0,-5,28}, {329,5,5,0,-5,36}, {342,5,5,0,-5,36}, {355,2,5,0,-5,14},
    {360,4,5,0,-5,28}, {370,5,5,0,-5,34}, {383,4,5,0,-5,27}, {393,6,5,0,-5,45}, {408,5,5,0,-5,37}, {421,5,5,0,-5,37},
    {434,4,5,0,-5,31}, {444,5,6,0,-5,37}, {459,4,5,0,-5,32}, {469,4,5,0,-5,31}, {479,4,5,0,-5,32}, {489,4,5,0,-5,35},
    {499,5,5,0,-5,36}, {512,7,5,0,-5,50}, {530,5,5,0,-5,36}, {543,5,5,0,-5,35}, {556,4,5,0,-5,32}, {566,2,6,0,-5,18},
    {572,3,6,0,-5,19}, {581,2,6,0,-5,18}, {587,3,2,0,-4,23}, {590,3,1,0,0,23}, {592,2,1,0,-5,18}, {593,4,4,0,-4,28},
    {601,4,5,0,-5,30}, {611,4,4,0,-4,28}, {619,4,5,0,-5,30}, {629,4,4,0,-4,29}, {637,3,6,0,-6,19}, {646,4,5,0,-4,30},
    {656,4,5,0,-5,30}, {666,2,6,0,-6,13}, {672,3,7,-1,-6,13}, {683,4,5,0,-5,28}, {693,2,5,0,-5,13}, {698,6,4,0,-4,44},
    {710,4,4,0,-4,30}, {718,4,4,0,-4,30}, {726,4,6,0,-4,30}, {738,4,6,0,-4,30}, {750,3,4,0,-4,20}, {756,4,4,0,-4,27},
    {764,3,5,0,-5,18}, {772,4,4,0,-4,30}, {780,4,4,0,-4,29}, {788,5,4,0,-4,41}, {798,4,4,0,-4,28}, {806,4,5,0,-4,29},
    {816,4,4,0,-4,28}, {824,3,6,0,-5,22}, {833,2,8,0,-6,18}, {841,3,6,0,-5,22}, {850,4,2,0,-3,33}, {854,0,0,0,0,16},
    {854,2,6,0,-6,21}, {860,4,3,0,-6,34}, {866,5,6,0,-6,41}, {881,5,8,0,-7,42}, {901,8,6,0,-6,64}, {925,6,6,0,-6,42},
    {943,2,3,0,-6,21}, {946,3,8,0,-7,24}, {958,3,8,0,-7,24}, {970,4,4,0,-6,34}, {978,5,5,0,-5,43}, {991,2,3,0,-1,20},
    {994,4,1,0,-3,30}, {996,2,2,0,-2,20}, {998,3,7,0,-6,24}, {1009,5,6,0,-6,42}, {1024,3,6,0,-6,27}, {1033,5,6,0,-6,40},
    {1048,5,6,0,-6,41}, {1063,5,6,0,-6,43}, {1078,5,6,0,-6,40}, {1093,5,6,0,-6,41}, {1108,5,6,0,-6,37}, {1123,5,6,0,-6,41},
    {1138,5,6,0,-6,41}, {1153,2,5,0,-5,20}, {1158,2,6,0,-4,21}, {1164,5,5,0,-5,43}, {1177,5,3,0,-4,43}, {1185,5,5,0,-5,43},
    {1198,4,6,0,-6,35}, {1210,8,8,0,-6,64}, {1242,6,6,0,-6,47}, {1260,5,6,0,-6,42}, {1275,6,6,0,-6,47}, {1293,6,6,0,-6,46},
    {1311,5,6,0,-6,39}, {1326,5,6,0,-6,38}, {1341,6,6,0,-6,48}, {1359,6,6,0,-6,48}, {1377,2,6,0,-6,18}, {1383,5,6,0,-6,37},
    {1398,6,6,0,-6,45}, {1416,5,6,0,-6,36}, {1431,7,6,0,-6,59}, {1452,6,6,0,-6,49}, {1470,6,6,0,-6,49}, {1488,5,6,0,-6,41},
    {1503,6,7,0,-6,50}, {1524,5,6,0,-6,42}, {1539,5,6,0,-6,42}, {1554,5,6,0,-6,42}, {1569,6,6,0,-6,47}, {1587,6,6,0,-6,47},
    {1605,8,6,0,-6,65}, {1629,6,6,0,-6,46}, {1647,6,6,0,-6,46}, {1665,5,6,0,-6,42}, {1680,3,7,0,-6,24}, {1691,3,7,0,-6,24},
    {1702,3,7,0,-6,24}, {1713,4,3,0,-6,31}, {1719,4,1,0,0,30}, {1721,3,2,0,-7,22}, {1724,5,5,0,-5,37}, {1737,5,6,0,-6,40},
    {1752,5,5,0,-5,37}, {1765,5,6,0,-6,40}, {1780,5,5,0,-5,38}, {1793,3,7,0,-7,25}, {1804,5,7,0,-5,40}, {1822,5,6,0,-6,39},
    {1837,2,7,0,-7,17}, {1844,3,9,-1,-7,17}, {1858,5,6,0,-6,36}, {1873,2,6,0,-6,17}, {1879,7,5,0,-5,58}, {1897,5,5,0,-5,39},
    {1910,5,5,0,-5,39}, {1923,5,7,0,-5,40}, {1941,5,7,0,-5,40}, {1959,4,5,0,-5,25}, {1969,5,5,0,-5,35}, {1982,3,7,0,-7,23},
    {1993,5,5,0,-5,39}, {2006,5,5,0,-5,38}, {2019,7,5,0,-5,54}, {2037,5,5,0,-5,36}, {2050,5,7,0,-5,38}, {2068,5,5,0,-5,36},
    {2081,4,8,0,-7,29}, {2097,2,10,0,-8,23}, {2107,4,7,0,-6,29}, {2121,5,2,0,-3,43}, {2126,0,0,0,0,15}, {2126,3,6,0,-6,22},
    {2135,4,3,0,-6,35}, {2141,6,6,0,-6,42}, {2159,5,8,0,-7,42}, {2179,8,6,0,-6,65}, {2203,6,6,0,-6,43}, {2221,2,3,0,-6,22},
    {2224,3,8,0,-7,24}, {2236,3,8,0,-7,24}, {2248,4,4,0,-6,36}, {2256,5,5,0,-5,44}, {2269,2,3,0,-1,21}, {2272,4,1,0,-3,30},
    {2274,2,2,0,-2,21}, {2276,3,7,0,-6,25}, {2287,5,6,0,-6,43}, {2302,3,6,0,-6,28}, {2311,5,6,0,-6,40}, {2326,5,6,0,-6,41},
    {2341,6,6,0,-6,43}, {2359,5,6,0,-6,41}, {2374,5,6,0,-6,42}, {2389,5,6,0,-6,37}, {2404,5,6,0,-6,42}, {2419,5,6,0,-6,42},
    {2434,2,5,0,-5,21}, {2439,3,6,0,-4,22}, {2448,5,5,0,-5,44}, {2461,5,3,0,-4,44}, {2469,5,5,0,-5,44}, {2482,5,6,0,-6,36},
    {2497,8,8,0,-6,65}, {2529,6,6,0,-6,48}, {2547,5,6,0,-6,42}, {2562,6,6,0,-6,47}, {2580,6,6,0,-6,46}, {2598,5,6,0,-6,39},
    {2613,5,6,0,-6,38}, {2628,6,6,0,-6,48}, {2646,6,6,0,-6,48}, {2664,2,6,0,-6,18}, {2670,5,6,0,-6,37}, {2685,6,6,0,-6,46},
    {2703,5,6,0,-6,36}, {2718,7,6,0,-6,60}, {2739,6,6,0,-6,49}, {2757,6,6,0,-6,49}, {2775,5,6,0,-6,42}, {2790,6,7,0,-6,50},
    {2811,6,6,0,-6,42}, {2829,5,6,0,-6,42}, {2844,6,6,0,-6,43}, {2862,6,6,0,-6,47}, {2880,6,6,0,-6,48}, {2898,9,6,0,-6,66},
    {2925,6,6,0,-6,47}, {2943,6,6,0,-6,47}, {2961,5,6,0,-6,42}, {2976,3,7,0,-6,24}, {2987,3,7,0,-6,25}, {2998,3,7,0,-6,24},
    {3009,4,3,0,-6,31}, {3015,4,1,0,0,30}, {3017,3,2,0,-7,23}, {3020,5,5,0,-5,37}, {3033,5,6,0,-6,40}, {3048,5,5,0,-5,38},
    {3061,5,6,0,-6,40}, {3076,5,5,0,-5,38}, {3089,4,7,0,-7,26}, {3103,5,7,0,-5,40}, {3121,5,6,0,-6,40}, {3136,2,7,0,-7,17},
    {3143,3,9,-1,-7,17}, {3157,5,6,0,-6,37}, {3172,2,6,0,-6,17}, {3178,7,5,0,-5,58}, {3196,5,5,0,-5,40}, {3209,5,5,0,-5,39},
    {3222,5,7,0,-5,40}, {3240,5,7,0,-5,40}, {3258,4,5,0,-5,26}, {3268,5,5,0,-5,36}, {3281,3,7,0,-7,24}, {3292,5,5,0,-5,40},
    {3305,5,5,0,-5,38}, {3318,7,5,0,-5,54}, {3336,5,5,0,-5,37}, {3349,5,7,0,-5,38}, {3367,5,5,0,-5,37}, {3380,4,7,0,-6,30},
    {3394,3,10,0,-8,24}, {3409,4,7,0,-6,30}, {3423,5,2,0,-3,44}, {3428,0,0,0,0,17}, {3428,3,7,0,-7,24}, {3439,5,3,0,-7,40},
    {3447,6,7,0,-7,47}, {3468,6,9,0,-8,47}, {3495,9,7,0,-7,73}, {3527,6,7,0,-7,48}, {3548,3,3,0,-7,24}, {3553,4,9,0,-7,27},
    {3571,3,9,0,-7,27}, {3585,5,4,0,-7,40}, {3595,6,5,0,-5,49}, {3610,3,3,0,-1,24}, {3615,4,2,0,-4,34}, {3619,3,2,0,-2,24},
    {3622,4,8,0,-7,28}, {3638,6,7,0,-7,48}, {3659,4,7,0,-7,31}, {3673,6,7,0,-7,45}, {3694,6,7,0,-7,46}, {3715,6,7,0,-7,49},
    {3736,6,7,0,-7,46}, {3757,6,7,0,-7,47}, {3778,5,7,0,-7,42}, {3796,6,7,0,-7,47}, {3817,6,7,0,-7,47}, {3838,3,5,0,-5,24},
    {3846,3,7,0,-5,25}, {3857,6,6,0,-6,49}, {3875,6,4,0,-5,49}, {3887,6,6,0,-6,49}, {3905,5,7,0,-7,40}, {3923,9,9,0,-7,73},
    {3964,7,7,0,-7,54}, {3989,6,7,0,-7,48}, {4010,7,7,0,-7,53}, {4035,7,7,0,-7,52}, {4060,5,7,0,-7,44}, {4078,5,7,0,-7,42},
    {4096,7,7,0,-7,54}, {4121,7,7,0,-7,54}, {4146,2,7,0,-7,20}, {4153,5,7,0,-7,42}, {4171,7,7,0,-7,52}, {4196,5,7,0,-7,41},
    {4214,8,7,0,-7,67}, {4242,7,7,0,-7,55}, {4267,7,7,0,-7,56}, {4292,6,7,0,-7,47}, {4313,7,8,0,-7,56}, {4341,6,7,0,-7,47},
    {4362,6,7,0,-7,47}, {4383,6,7,0,-7,48}, {4404,6,7,0,-7,53}, {4425,7,7,0,-7,54}, {4450,10,7,0,-7,75}, {4485,7,7,0,-7,53},
    {4510,7,7,0,-7,53}, {4535,6,7,0,-7,48}, {4556,3,9,0,-7,27}, {4570,4,8,0,-7,28}, {4586,3,9,0,-7,27}, {4600,5,3,0,-6,35},
    {4608,5,2,0,0,34}, {4613,3,2,0,-7,26}, {4616,5,5,0,-5,42}, {4629,6,7,0,-7,45}, {4650,5,5,0,-5,42}, {4663,6,7,0,-7,45},
    {4684,6,5,0,-5,43}, {4699,4,7,0,-7,29}, {4713,6,7,0,-5,46}, {4734,6,7,0,-7,45}, {4755,2,7,0,-7,20}, {4762,3,9,-1,-7,20},
    {4776,6,7,0,-7,42}, {4797,2,7,0,-7,20}, {4804,8,5,0,-5,66}, {4824,6,5,0,-5,45}, {4839,6,5,0,-5,44}, {4854,6,7,0,-5,45},
    {4875,6,7,0,-5,45}, {4896,4,5,0,-5,29}, {4906,5,5,0,-5,40}, {4919,4,7,0,-7,26}, {4933,6,5,0,-5,45}, {4948,6,5,0,-5,43},
    {4963,8,5,0,-5,61}, {4983,5,5,0,-5,42}, {4996,6,7,0,-5,43}, {5017,5,5,0,-5,41}, {5030,4,8,0,-7,34}, {5046,2,11,1,-9,27},
    {5057,4,8,0,-7,34}, {5073,6,3,0,-4,49}, {5082,0,0,0,0,20}, {5082,3,8,0,-8,26}, {5094,4,3,1,-8,42}, {5100,7,8,0,-8,52},
    {5128,7,10,0,-9,52}, {5163,10,8,0,-8,80}, {5203,7,8,0,-8,53}, {5231,2,3,1,-8,26}, {5234,4,10,0,-8,30}, {5254,3,10,0,-8,30},
    {5269,5,5,0,-8,43}, {5282,6,5,0,-5,54}, {5297,3,3,0,-1,26}, {5302,4,2,0,-4,37}, {5306,3,2,0,-2,26}, {5309,4,9,0,-8,30},
    {5327,7,8,0,-8,53}, {5355,4,8,0,-8,34}, {5371,6,8,0,-8,50}, {5395,6,8,0,-8,51}, {5419,7,8,0,-8,53}, {5447,6,8,0,-8,50},
    {5471,6,8,0,-8,51}, {5495,6,8,0,-8,46}, {5519,6,8,0,-8,51}, {5543,6,8,0,-8,51}, {5567,3,6,0,-6,26}, {5576,3,8,0,-6,26},
    {5588,6,6,0,-6,54}, {5606,5,4,1,-5,54}, {5616,5,6,1,-6,54}, {5631,5,8,0,-8,44}, {5651,10,9,0,-7,80}, {5696,7,8,0,-8,58},
    {5724,7,8,0,-8,53}, {5752,7,8,0,-8,59}, {5780,7,8,0,-8,58}, {5808,6,8,0,-8,48}, {5832,6,8,0,-8,47}, {5856,7,8,0,-8,60},
    {5884,7,8,0,-8,60}, {5912,3,8,0,-8,22}, {5924,6,8,0,-8,46}, {5948,7,8,0,-8,56}, {5976,6,8,0,-8,45}, {6000,9,8,0,-8,74},
    {6036,7,8,0,-8,61}, {6064,8,8,0,-8,62}, {6096,6,8,0,-8,52}, {6120,8,9,0,-8,62}, {6156,7,8,0,-8,52}, {6184,7,8,0,-8,52},
    {6212,7,8,0,-8,53}, {6240,7,8,0,-8,59}, {6268,7,8,0,-8,58}, {6296,10,8,0,-8,82}, {6336,7,8,0,-8,58}, {6364,7,8,0,-8,57},
    {6392,6,8,0,-8,52}, {6416,4,9,0,-8,30}, {6434,4,9,0,-8,30}, {6452,3,9,0,-8,30}, {6466,5,4,0,-7,38}, {6476,5,2,0,0,38},
    {6481,3,2,0,-8,28}, {6484,6,6,0,-6,46}, {6502,6,8,0,-8,50}, {6526,6,6,0,-6,47}, {6544,6,8,0,-8,50}, {6568,6,6,0,-6,47},
    {6586,4,9,0,-9,31}, {6604,6,8,0,-6,50}, {6628,6,8,0,-8,49}, {6652,3,8,0,-8,21}, {6664,4,11,-1,-8,21}, {6686,6,8,0,-8,46},
    {6710,2,8,0,-8,21}, {6718,9,6,0,-6,72}, {6745,6,6,0,-6,49}, {6763,6,6,0,-6,49}, {6781,6,8,0,-6,50}, {6805,6,8,0,-6,50},
    {6829,4,6,0,-6,32}, {6841,6,6,0,-6,44}, {6859,4,8,0,-8,28}, {6875,6,6,0,-6,49}, {6893,6,6,0,-6,47}, {6911,9,6,0,-6,67},
    {6938,6,6,0,-6,46}, {6956,6,9,0,-6,47}, {6983,6,6,0,-6,45}, {7001,4,9,0,-8,36}, {7019,2,12,1,-10,29}, {7031,4,9,0,-8,36},
    {7049,6,3,0,-4,54}, {7058,0,0,0,0,23}, {7058,3,9,1,-9,32}, {7072,5,4,1,-9,53}, {7082,8,9,0,-9,62}, {7118,8,13,0,-11,63},
    {7170,11,10,1,-9,98}, {7225,8,9,0,-9,64}, {7261,2,4,1,-9,32}, {7265,4,11,1,-9,36}, {7287,4,11,0,-9,36}, {7309,6,5,0,-9,54},
    {7324,7,7,1,-7,65}, {7349,3,4,0,-2,32}, {7355,5,2,0,-5,45}, {7360,2,2,1,-2,32}, {7362,5,11,0,-9,37}, {7390,8,9,0,-9,65},
    {7426,5,9,1,-9,65}, {7449,7,9,0,-9,65}, {7481,8,9,0,-9,65}, {7517,8,9,0,-9,65}, {7553,8,9,0,-9,65}, {7589,8,9,0,-9,65},
    {7625,7,9,1,-9,65}, {7657,8,9,0,-9,65}, {7693,8,9,0,-9,65}, {7729,2,6,1,-6,32}, {7735,4,9,0,-6,33}, {7753,7,7,0,-7,65},
    {7778,6,5,1,-6,65}, {7793,7,7,1,-7,65}, {7818,7,9,0,-9,54}, {7850,12,12,0,-9,98}, {7922,9,9,0,-9,72}, {7963,8,9,0,-9,64},
    {7999,9,9,0,-9,71}, {8040,9,9,0,-9,69}, {8081,7,9,0,-9,58}, {8113,7,9,0,-9,56}, {8145,9,9,0,-9,72}, {8186,9,9,0,-9,72},
    {8227,3,9,0,-9,27}, {8241,7,9,0,-9,56}, {8273,9,9,0,-9,69}, {8314,7,9,0,-9,54}, {8346,11,9,0,-9,90}, {8396,9,9,0,-9,73},
    {8437,9,9,0,-9,74}, {8478,8,9,0,-9,62}, {8514,9,10,0,-9,75}, {8559,8,9,0,-9,63}, {8595,8,9,0,-9,63}, {8631,8,9,0,-9,64},
    {8667,8,9,0,-9,70}, {8703,9,9,0,-9,72}, {8744,13,9,0,-9,100}, {8803,9,9,0,-9,71}, {8844,9,9,0,-9,70}, {8885,8,9,0,-9,64},
    {8921,3,12,1,-10,36}, {8939,5,11,0,-9,37}, {8967,4,12,0,-10,36}, {8991,6,5,0,-9,47}, {9006,6,2,0,0,46}, {9012,3,2,1,-9,35},
    {9015,7,7,0,-7,56}, {9040,8,9,0,-9,60}, {9076,7,7,0,-7,56}, {9101,7,9,0,-9,60}, {9133,7,7,0,-7,57}, {9158,5,10,0,-10,38},
    {9183,7,10,0,-7,61}, {9218,7,9,0,-9,60}, {9250,3,10,0,-10,26}, {9265,4,13,-1,-10,26}, {9291,7,9,0,-9,56}, {9323,3,9,0,-9,26},
    {9337,11,7,0,-7,88}, {9376,7,7,0,-7,60}, {9401,7,7,0,-7,59}, {9426,8,10,0,-7,60}, {9466,7,10,0,-7,60}, {9501,5,7,0,-7,39},
    {9519,7,7,0,-7,54}, {9544,5,9,0,-9,35}, {9567,7,7,0,-7,60}, {9592,7,7,0,-7,58}, {9617,10,7,0,-7,82}, {9652,7,7,0,-7,56},
    {9677,7,10,0,-7,58}, {9712,7,7,0,-7,55}, {9737,6,12,0,-10,45}, {9773,3,15,1,-12,36}, {9796,5,12,0,-10,45}, {9826,8,3,0,-5,65},
    {9838,8,8,0,-8,66}, {9870,10,8,0,-8,82}, {9910,7,8,0,-8,57}, {9938,13,16,1,-16,119}, {10042,9,16,3,-16,119}, {10114,12,16,2,-16,119},
    {10210,13,16,1,-16,119}, {10314,13,16,1,-16,119}, {10418,13,16,1,-16,119}, {10522,13,16,1,-16,119}, {10626,12,16,1,-16,119}, {10722,13,16,1,-16,119},
    {10826,13,16,1,-16,119}, {10930,5,12,1,-12,59},
};
enum { F_6B, F_8S, F_8B, F_9B, F_10S, F_12B, F_11B, F_22B, F_COUNT };
static const PFont kPFonts[F_COUNT] = {
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 0, 6 }, /* F_6B 6px */
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 95, 7 }, /* F_8S 8px */
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 190, 7 }, /* F_8B 8px */
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 285, 8 }, /* F_9B 9px */
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 380, 9 }, /* F_10S 10px */
    { " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 475, 11 }, /* F_12B 12px */
    { "AMP", 570, 10 }, /* F_11B 11px */
    { "0123456789:", 573, 21 }, /* F_22B 22px */
};

/* the panel's little vector icons (the design's SVGs, rasterized at their size on the panel; RGBA) */
static const u8 kIcon_eye[20 * 14 * 4] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,1,255,255,255,49,255,255,255,80,
    255,255,255,99,255,255,255,119,255,255,255,119,255,255,255,99,255,255,255,80,255,255,255,48,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,54,255,255,255,160,255,255,255,237,255,255,255,255,255,255,255,255,255,255,255,255,254,254,255,255,254,254,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,235,255,255,255,158,255,255,255,47,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,14,255,255,255,159,255,255,255,255,
    255,255,255,226,255,255,255,151,255,255,255,137,255,255,255,255,243,242,250,254,200,196,235,255,200,196,235,255,244,243,251,254,255,255,255,255,255,255,255,135,255,255,255,152,255,255,255,227,
    255,255,255,255,255,255,255,159,255,255,255,13,0,0,0,0,255,255,255,5,255,255,255,193,255,255,255,255,255,255,255,154,255,255,255,10,0,0,0,0,254,254,254,189,252,252,254,255,
    112,103,202,255,96,88,184,255,96,88,184,255,113,104,203,255,252,252,254,255,254,254,254,182,0,0,0,0,255,255,255,12,255,255,255,155,255,255,255,255,255,255,255,192,255,255,255,5,
    255,255,255,152,255,255,255,255,255,255,255,124,0,0,0,0,0,0,0,0,0,0,0,0,253,253,254,229,211,208,239,255,101,92,192,255,12,17,54,255,12,17,54,255,100,93,192,255,
    213,210,240,255,252,252,254,225,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,129,255,255,255,255,255,255,255,148,255,255,255,151,255,255,255,255,255,255,255,125,0,0,0,0,
    0,0,0,0,0,0,0,0,253,253,254,229,214,211,240,255,100,93,192,255,12,17,54,255,12,17,54,255,101,92,192,255,215,212,240,255,252,252,254,228,0,0,0,0,0,0,0,0,
    0,0,0,0,255,255,255,117,255,255,255,254,255,255,255,150,255,255,255,5,255,255,255,198,255,255,255,255,255,255,255,174,255,255,255,35,0,0,0,0,254,254,254,189,253,253,254,255,
    126,118,208,255,96,88,184,255,96,88,184,255,129,121,209,255,253,253,254,255,254,254,254,184,0,0,0,0,255,255,255,38,255,255,255,177,255,255,255,255,255,255,255,193,255,255,255,5,
    0,0,0,0,255,255,255,16,255,255,255,159,255,255,255,255,255,255,255,254,255,255,255,193,255,255,255,149,255,255,255,255,254,254,255,255,209,206,238,255,212,209,239,255,254,254,255,255,
    255,255,255,255,255,255,255,148,255,255,255,195,255,255,255,255,255,255,255,255,255,255,255,159,255,255,255,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,54,
    255,255,255,164,255,255,255,234,255,255,255,255,255,255,255,255,255,255,255,255,254,254,255,255,254,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,235,255,255,255,158,
    255,255,255,47,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,1,255,255,255,49,255,255,255,80,
    255,255,255,100,255,255,255,119,255,255,255,119,255,255,255,99,255,255,255,80,255,255,255,49,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const u8 kIcon_gear[15 * 15 * 4] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,64,255,255,255,255,255,255,255,64,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,22,255,255,255,123,0,0,0,0,0,0,0,0,
    255,255,255,64,255,255,255,255,255,255,255,64,0,0,0,0,0,0,0,0,255,255,255,123,255,255,255,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,104,
    255,255,255,255,255,255,255,144,255,255,255,142,255,255,255,222,255,255,255,250,255,255,255,221,255,255,255,139,255,255,255,144,255,255,255,255,255,255,255,103,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,133,255,255,255,230,255,255,255,251,255,255,255,179,255,255,255,140,255,255,255,179,255,255,255,251,255,255,255,229,255,255,255,132,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,128,255,255,255,249,255,255,255,53,255,255,255,114,255,255,255,174,255,255,255,113,
    255,255,255,52,255,255,255,249,255,255,255,122,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,4,255,255,255,64,255,255,255,64,255,255,255,213,255,255,255,168,255,255,255,89,
    255,255,255,255,255,255,255,224,255,255,255,255,255,255,255,86,255,255,255,168,255,255,255,206,255,255,255,64,255,255,255,64,255,255,255,4,255,255,255,16,255,255,255,255,255,255,255,255,
    255,255,255,247,255,255,255,136,255,255,255,146,255,255,255,236,0,0,0,0,255,255,255,238,255,255,255,145,255,255,255,136,255,255,255,245,255,255,255,255,255,255,255,255,255,255,255,16,
    255,255,255,4,255,255,255,64,255,255,255,64,255,255,255,210,255,255,255,168,255,255,255,76,255,255,255,255,255,255,255,224,255,255,255,255,255,255,255,73,255,255,255,169,255,255,255,207,
    255,255,255,64,255,255,255,64,255,255,255,4,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,125,255,255,255,249,255,255,255,40,255,255,255,72,255,255,255,166,255,255,255,72,
    255,255,255,45,255,255,255,249,255,255,255,120,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,134,255,255,255,230,255,255,255,251,
    255,255,255,178,255,255,255,139,255,255,255,180,255,255,255,252,255,255,255,228,255,255,255,132,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,104,
    255,255,255,255,255,255,255,143,255,255,255,140,255,255,255,222,255,255,255,250,255,255,255,221,255,255,255,139,255,255,255,145,255,255,255,255,255,255,255,103,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,255,255,255,24,255,255,255,125,0,0,0,0,0,0,0,0,255,255,255,64,255,255,255,255,255,255,255,64,0,0,0,0,0,0,0,0,255,255,255,122,
    255,255,255,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,64,255,255,255,255,255,255,255,64,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const u8 kIcon_ocarina[20 * 14 * 4] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,255,255,255,130,254,254,254,252,251,251,254,182,255,255,255,37,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,48,255,255,255,255,181,188,231,255,
    210,213,241,250,254,254,255,253,255,255,255,118,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,1,255,255,255,30,255,255,255,69,
    255,255,255,109,255,255,255,109,255,255,255,69,255,255,255,30,0,0,0,0,251,251,254,194,219,222,243,252,127,138,214,255,127,138,214,255,179,185,231,255,254,254,255,252,255,255,255,1,
    0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,44,246,246,253,147,245,245,252,240,223,226,244,241,211,214,240,244,197,201,237,250,197,201,237,250,211,215,241,244,224,226,244,241,
    245,246,253,240,255,255,255,254,173,180,229,255,127,138,214,255,127,138,214,255,221,224,244,252,251,251,254,192,0,0,0,0,0,0,0,0,0,0,0,0,245,245,252,151,252,253,254,253,
    199,204,237,252,138,148,217,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,139,149,218,255,226,229,246,253,254,254,255,255,209,213,240,254,
    179,186,231,255,255,255,255,255,255,255,255,46,0,0,0,0,0,0,0,0,251,251,254,170,248,249,252,252,152,161,222,255,127,138,214,255,127,138,214,255,127,138,214,255,46,53,101,255,
    49,55,105,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,169,175,227,255,250,251,253,254,255,255,255,255,255,255,255,140,0,0,0,0,0,0,0,0,
    255,255,255,41,255,255,255,255,152,161,222,255,127,138,214,255,46,53,101,255,49,55,105,255,127,138,214,255,49,55,105,255,49,55,105,255,127,138,214,255,46,53,101,255,49,55,105,255,
    127,138,214,255,127,138,214,255,127,138,214,255,152,161,222,255,255,255,255,255,255,255,255,40,0,0,0,0,0,0,0,0,255,255,255,116,229,231,247,246,127,138,214,255,127,138,214,255,
    49,55,105,255,49,55,105,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,49,55,105,255,49,55,105,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,
    230,232,248,246,255,255,255,115,0,0,0,0,0,0,0,0,255,255,255,115,226,228,246,245,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,
    127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,227,229,246,245,255,255,255,114,0,0,0,0,0,0,0,0,
    255,255,255,38,254,254,255,253,137,147,217,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,46,53,101,255,49,55,105,255,127,138,214,255,127,138,214,255,127,138,214,255,
    127,138,214,255,127,138,214,255,127,138,214,255,137,147,217,255,254,254,255,253,255,255,255,37,0,0,0,0,0,0,0,0,0,0,0,0,246,246,251,137,243,244,251,248,148,157,221,255,
    127,138,214,255,127,138,214,255,127,138,214,255,49,55,105,255,49,55,105,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,148,157,221,255,243,244,251,248,
    246,247,253,135,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,251,251,253,123,253,254,255,252,213,216,242,249,157,166,224,255,127,138,214,255,127,138,214,255,
    127,138,214,255,127,138,214,255,127,138,214,255,127,138,214,255,158,167,224,255,213,217,242,249,253,253,254,253,251,251,253,119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,52,254,254,254,172,255,255,255,255,250,250,253,248,231,233,248,240,202,206,238,247,204,210,240,247,230,233,247,241,250,250,253,249,
    255,255,255,254,253,253,253,169,255,255,255,52,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,255,255,255,33,255,255,255,79,255,255,255,99,255,255,255,119,255,255,255,118,255,255,255,99,255,255,255,79,255,255,255,31,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const u8 kIcon_sun[13 * 13 * 4] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,255,211,77,255,255,211,77,255,
    255,211,77,255,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,
    255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,240,168,255,255,240,168,255,255,240,168,255,255,240,168,255,
    255,240,168,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,255,240,168,255,255,240,168,255,255,240,168,255,
    255,240,168,255,255,240,168,255,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,255,240,168,255,255,240,168,255,
    255,240,168,255,255,240,168,255,255,240,168,255,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,255,240,168,255,
    255,240,168,255,255,240,168,255,255,240,168,255,255,240,168,255,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    255,240,168,255,255,240,168,255,255,240,168,255,255,240,168,255,255,240,168,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,
    255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,255,211,77,255,
    255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,
    255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,211,77,255,255,211,77,255,255,211,77,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,
};
static const u8 kIcon_night[12 * 12 * 4] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,201,210,255,255,201,210,255,255,201,210,255,255,201,210,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static const PGlyph* PGlyphOf(int f, unsigned char c) {
    const char* p = strchr(kPFonts[f].chars, c);
    if (c == 0 || p == NULL) return NULL;
    return &kPGlyphs[kPFonts[f].first + (p - kPFonts[f].chars)];
}
/* the width of s in font f, ls8 extra eighths of a pixel between letters */
static int PTextW8(int f, const char* s, int ls8) {
    int w8 = 0, n = 0;
    for (; *s; s++, n++) {
        const PGlyph* g = PGlyphOf(f, (unsigned char)*s);
        w8 += g != NULL ? g->adv8 : kPFonts[f].asc * 8 / 3;
    }
    return (w8 + (n > 1 ? (n - 1) * ls8 : 0) + 4) / 8;
}
static int PTextW(int f, const char* s, int ls) { /* ls whole pixels */
    return PTextW8(f, s, ls * 8);
}
static void PGlyphDraw(const PGlyph* g, int x, int base, u32 rgb) {
    int i, j, r = rgb >> 16, gg = (rgb >> 8) & 255, b = rgb & 255;
    for (j = 0; j < g->h; j++) {
        for (i = 0; i < g->w; i++) {
            int k = j * g->w + i, v = (kPFontBits[g->off + (k >> 1)] >> ((k & 1) ? 0 : 4)) & 15;
            if (v) PxA(x + g->bx + i, base + g->by + j, r, gg, b, v * 17);
        }
    }
}
/* text at x (left), y the top of its line (the design's CSS boxes: the baseline sits the font's ascent below), with a
 * 1-pixel drop shadow (the design's 2 px at 2x) */
static void PTextB(int x, int base, const char* s, int f, u32 rgb, int shadow, int ls8) {
    int pass;
    for (pass = shadow ? 0 : 1; pass < 2; pass++) {
        int x8 = x * 8;
        const char* t;
        for (t = s; *t; t++) {
            const PGlyph* g = PGlyphOf(f, (unsigned char)*t);
            if (g == NULL) {
                x8 += kPFonts[f].asc * 8 / 3 + ls8;
                continue;
            }
            if (g->w) PGlyphDraw(g, (x8 + 4) / 8 + (pass == 0), base + (pass == 0), pass == 0 ? ColShadow() : rgb);
            x8 += g->adv8 + ls8;
        }
    }
}
static void PText(int x, int y, const char* s, int f, u32 rgb, int shadow, int ls) { /* ls whole pixels */
    PTextB(x, y + kPFonts[f].asc, s, f, rgb, shadow, ls * 8);
}
static void PTextC(int cx, int y, const char* s, int f, u32 rgb, int shadow) { /* centred on cx */
    PText(cx - PTextW(f, s, 0) / 2, y, s, f, rgb, shadow, 0);
}
static void PTextR(int rx, int y, const char* s, int f, u32 rgb, int shadow) { /* ending at rx */
    PText(rx - PTextW(f, s, 0), y, s, f, rgb, shadow, 0);
}
/* centred in a box h pixels tall at y (the font's capitals in the middle) */
static void PTextMid(int cx, int y, int h, const char* s, int f, u32 rgb, int shadow) {
    int cap = kPFonts[f].asc * 3 / 4; /* about the capitals' height */
    PTextB(cx - PTextW(f, s, 0) / 2, y + (h + cap + 1) / 2, s, f, rgb, shadow, 0);
}

/* ---- the game's art, read from the ROM once (PORT 2026-11-06) ----
 * The frames, the buttons, the hearts, the magic meter, the rupee, the day plates, the remains and the C-button sockets
 * are the game's own textures (the file select's title_static, the HUD's parameter_static, the moon's object_fall),
 * read from the ROM file as stored (big-endian: PortDma_ReadRomRaw, not the natively compiled copies) and decoded here,
 * then drawn 1:1 where the design puts them, tinted like the file select screen does. */
extern void PortDma_ReadRomRaw(void* dst, u32 offset, u32 n);
extern unsigned char _title_staticSegmentRomStart[], _parameter_staticSegmentRomStart[], _object_fallSegmentRomStart[];
#define ROM_TITLE ((u32)(uintptr_t)_title_staticSegmentRomStart)
#define ROM_PARAM ((u32)(uintptr_t)_parameter_staticSegmentRomStart)
#define ROM_FALL ((u32)(uintptr_t)_object_fallSegmentRomStart)
typedef struct {
    s16 w, h;
    u8 rgba; /* 0: intensity, alpha pairs (tinted when drawn); 1: RGBA */
    u8* px;
} PArt;
enum { A_WINDOW, A_INFO, A_DAYBOX, A_BLANK, A_HILITE, A_SOCKET, A_METER_END, A_METER_MID, A_DAY1, A_DAY2, A_DAY3, A_RUPEE,
       A_KEY, A_DIGITS /* the HUD's ammo digits 0..9, 8 x 8 each */, A_HEART0, /* empty, 1/4, 1/2, 3/4, full (10 x 10) */
       A_HEARTIA0 = A_HEART0 + 5, /* the same five 16 x 16, intensity / alpha (the beating heart is scaled from them) */
       A_REMAINS0 = A_HEARTIA0 + 5, A_MOON = A_REMAINS0 + 4, A_MOON_BIG,
       A_COUNTER /* the HUD's counter digits 0..9 (the rupees), 8 x 16 each */,
       A_CLKDIG /* the three-day clock's hour digits as a font 0..9, a 16 x 11 cell each */, A_CLKSUN, A_CLKMOON,
       A_DHEART0, /* the double defense hearts, empty .. full (10 x 10), and the same 16 x 16 intensity / alpha */
       A_DHEARTIA0 = A_DHEART0 + 5, A_COUNT = A_DHEARTIA0 + 5 };
static PArt sArt[A_COUNT];
static u8 sArtPool[168 * 1024];
static int sArtUsed, sArtState; /* 0 not loaded, 1 loaded */
static u8 sRomTmp[4096];
enum { TF_IA16, TF_IA8, TF_I8, TF_I4, TF_RGBA16, TF_RGBA32 };

static PArt* ArtNew(int id, int w, int h, int rgba) {
    PArt* a = &sArt[id];
    int n = w * h * (rgba ? 4 : 2);
    a->w = w, a->h = h, a->rgba = rgba, a->px = NULL;
    if (sArtUsed + n <= (int)sizeof(sArtPool)) {
        a->px = sArtPool + sArtUsed;
        sArtUsed += n;
        memset(a->px, 0, n);
    }
    return a;
}
/* a w x h texture at rom + off into the art at (dx, dy) */
static void ArtLoad(PArt* a, int dx, int dy, u32 rom, u32 off, int w, int h, int fmt) {
    static const u8 kNibbles[] = { 4, 2, 2, 1, 4, 8 }; /* per texel */
    u32 size = ((u32)(w * h) * kNibbles[fmt] + 1) / 2;
    int i, j;
    if (a->px == NULL || size > sizeof(sRomTmp)) return;
    PortDma_ReadRomRaw(sRomTmp, rom + off, size);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            const u8* t = sRomTmp;
            int k = j * w + i, I = 0, A = 0, r, g, b;
            u8* d;
            if (dx + i >= a->w || dy + j >= a->h) continue;
            d = a->px + ((dy + j) * a->w + dx + i) * (a->rgba ? 4 : 2);
            switch (fmt) {
                case TF_IA16: I = t[2 * k], A = t[2 * k + 1]; break;
                case TF_IA8: I = (t[k] >> 4) * 17, A = (t[k] & 15) * 17; break;
                case TF_I8: I = A = t[k]; break;
                case TF_I4: I = A = ((t[k >> 1] >> ((k & 1) ? 0 : 4)) & 15) * 17; break;
                case TF_RGBA16: {
                    int v = (t[2 * k] << 8) | t[2 * k + 1];
                    r = ((v >> 11) & 31) * 255 / 31, g = ((v >> 6) & 31) * 255 / 31, b = ((v >> 1) & 31) * 255 / 31;
                    if (a->rgba) d[0] = r, d[1] = g, d[2] = b, d[3] = (v & 1) ? 255 : 0;
                    continue;
                }
                default:
                    if (a->rgba) d[0] = t[4 * k], d[1] = t[4 * k + 1], d[2] = t[4 * k + 2], d[3] = t[4 * k + 3];
                    continue;
            }
            if (a->rgba) {
                d[0] = d[1] = d[2] = I, d[3] = A;
            } else {
                d[0] = I, d[1] = A;
            }
        }
    }
}
/* an RGBA image shrunk into an RGBA art (area average, alpha-weighted: the hearts' 16 -> 10, the moon) */
static void ArtShrink(const u8* src, int sw, int sh, PArt* dst) {
    int i, j, x, y;
    if (dst->px == NULL) return;
    for (j = 0; j < dst->h; j++) {
        for (i = 0; i < dst->w; i++) {
            float x0 = (float)i * sw / dst->w, x1 = (float)(i + 1) * sw / dst->w;
            float y0 = (float)j * sh / dst->h, y1 = (float)(j + 1) * sh / dst->h;
            float acc[4] = { 0, 0, 0, 0 }, area = (x1 - x0) * (y1 - y0);
            u8* d = dst->px + (j * dst->w + i) * 4;
            for (y = (int)y0; y < sh && y < y1; y++) {
                float wy = fminf(y1, (float)(y + 1)) - fmaxf(y0, (float)y);
                for (x = (int)x0; x < sw && x < x1; x++) {
                    const u8* s = src + (y * sw + x) * 4;
                    float wgt = wy * (fminf(x1, (float)(x + 1)) - fmaxf(x0, (float)x)), al = s[3] * wgt;
                    acc[0] += s[0] * al, acc[1] += s[1] * al, acc[2] += s[2] * al, acc[3] += al;
                }
            }
            if (acc[3] > 0.0f) {
                d[0] = (u8)(acc[0] / acc[3]), d[1] = (u8)(acc[1] / acc[3]), d[2] = (u8)(acc[2] / acc[3]);
                d[3] = (u8)(acc[3] / area + 0.5f);
            }
        }
    }
}
/* the moon at the end of the three-day bar: the moon's own face, eye and teeth textures (object_fall) laid on a disc */
static void ArtMoon(void) {
    static u8 sBig[64 * 64 * 4], sEye[32 * 32 * 4], sTeeth[64 * 32 * 4];
    PArt eye = { 32, 32, 1, sEye }, teeth = { 64, 32, 1, sTeeth };
    u8 tlut[32], face[2048];
    int x, y, e;
    PortDma_ReadRomRaw(tlut, ROM_FALL + 0x8318, sizeof(tlut));   /* gMoonFaceTLUT */
    PortDma_ReadRomRaw(face, ROM_FALL + 0xA338, sizeof(face));   /* gMoonFaceTex, ci4 64 x 64 */
    ArtLoad(&eye, 0, 0, ROM_FALL, 0x8338, 32, 32, TF_RGBA16);    /* gMoonEyeTex */
    ArtLoad(&teeth, 0, 0, ROM_FALL, 0x8B38, 64, 32, TF_RGBA16);  /* gMoonTeethTex */
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float fx = (x + 0.5f - 32.0f) / 32.0f, fy = (y + 0.5f - 32.0f) / 32.0f, d2 = fx * fx + fy * fy, l;
            int idx = (face[(y * 64 + x) >> 1] >> ((x & 1) ? 0 : 4)) & 15, v = (tlut[idx * 2] << 8) | tlut[idx * 2 + 1];
            u8* d = sBig + (y * 64 + x) * 4;
            if (d2 >= 1.0f) {
                d[3] = 0;
                continue;
            }
            l = 0.55f + 0.6f * (sqrtf(1.0f - d2) * 0.7f - fx * 0.25f - fy * 0.3f);
            l = l < 0.3f ? 0.3f : (l > 1.15f ? 1.15f : l);
            d[0] = (u8)Clamp8((int)(((v >> 11) & 31) * 255 / 31 * l));
            d[1] = (u8)Clamp8((int)(((v >> 6) & 31) * 255 / 31 * l));
            d[2] = (u8)Clamp8((int)(((v >> 1) & 31) * 255 / 31 * l));
            d[3] = 255;
        }
    }
    for (e = 0; e < 3; e++) { /* the eyes (the right one mirrored), then the teeth */
        int cx = e == 0 ? 16 : e == 1 ? 48 : 32, cy = e < 2 ? 25 : 47, w = e < 2 ? 18 : 30, h = e < 2 ? 12 : 12;
        const PArt* t = e < 2 ? &eye : &teeth;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int sx = x * t->w / w, sy = y * t->h / h, px = cx - w / 2 + x, py = cy - h / 2 + y, a;
                const u8* s;
                u8* d = sBig + (py * 64 + px) * 4;
                if (e == 1) sx = t->w - 1 - sx;
                s = t->px + (sy * t->w + sx) * 4;
                a = s[3];
                if (e < 2) { /* the eye's glow only: its dark surround stays the face */
                    int lum = s[0] > s[1] ? s[0] : s[1];
                    a = a * (lum * 2 > 255 ? 255 : lum * 2) / 255;
                }
                if (d[3] == 0 || a == 0) continue;
                d[0] = (u8)(d[0] + (s[0] - d[0]) * a / 255);
                d[1] = (u8)(d[1] + (s[1] - d[1]) * a / 255);
                d[2] = (u8)(d[2] + (s[2] - d[2]) * a / 255);
            }
        }
    }
    ArtShrink(sBig, 64, 64, ArtNew(A_MOON, 16, 16, 1));
    ArtShrink(sBig, 64, 64, ArtNew(A_MOON_BIG, 18, 18, 1));
}
static void ArtLoadAll(void) {
    static const u32 kWindow[5][4] = { /* gFileSelWindow00Tex .. 43Tex: 64 + 64 + 64 + 48 by 5 x 32 */
        { 0xBBB0, 0xCBB0, 0xDBB0, 0xEBB0 }, { 0xF7B0, 0x107B0, 0x117B0, 0x127B0 }, { 0x133B0, 0x143B0, 0x153B0, 0x163B0 },
        { 0x16FB0, 0x17FB0, 0x18FB0, 0x19FB0 }, { 0x1ABB0, 0x1BBB0, 0x1CBB0, 0x1DBB0 } };
    static const u32 kInfo[5] = { 0x22E30, 0x23DF0, 0x24DB0, 0x25D70, 0x26D30 }; /* gFileSelFileInfoBox0..4Tex */
    static const u32 kRemains[4] = { 0x2BCB0, 0x2CCB0, 0x2DCB0, 0x2ECB0 };      /* gFileSel...RemainsTex */
    static const u16 kHearts[5] = { 0x0, 0x100, 0x200, 0x300, 0x400 };         /* gHeartEmptyTex .. gHeartFullTex */
    PArt* a;
    int r, c, k;
    sArtUsed = 0;
    a = ArtNew(A_WINDOW, 240, 160, 0);
    for (r = 0; r < 5; r++)
        for (c = 0; c < 4; c++) ArtLoad(a, c * 64, r * 32, ROM_TITLE, kWindow[r][c], c == 3 ? 48 : 64, 32, TF_IA16);
    a = ArtNew(A_INFO, 168, 56, 0);
    for (k = 0; k < 5; k++) ArtLoad(a, k * 36, 0, ROM_TITLE, kInfo[k], k == 4 ? 24 : 36, 56, TF_IA16);
    a = ArtNew(A_DAYBOX, 56, 56, 0); /* gFileSelFileExtraInfoBox0Tex, 1Tex */
    ArtLoad(a, 0, 0, ROM_TITLE, 0x287B0, 28, 56, TF_IA16);
    ArtLoad(a, 28, 0, ROM_TITLE, 0x293F0, 28, 56, TF_IA16);
    ArtLoad(ArtNew(A_BLANK, 52, 16, 0), 0, 0, ROM_TITLE, 0x2A030, 52, 16, TF_IA16);  /* gFileSelBlankButtonTex */
    ArtLoad(ArtNew(A_HILITE, 56, 24, 0), 0, 0, ROM_TITLE, 0x2B3B0, 56, 24, TF_I8);   /* ...MediumButtonHighlightTex */
    ArtLoad(ArtNew(A_DAY1, 48, 24, 0), 0, 0, ROM_TITLE, 0x30130, 48, 24, TF_I4);     /* gFileSelFirstDayENGTex */
    ArtLoad(ArtNew(A_DAY2, 48, 24, 0), 0, 0, ROM_TITLE, 0x30370, 48, 24, TF_I4);
    ArtLoad(ArtNew(A_DAY3, 48, 24, 0), 0, 0, ROM_TITLE, 0x305B0, 48, 24, TF_I4);
    ArtLoad(ArtNew(A_RUPEE, 16, 16, 0), 0, 0, ROM_TITLE, 0x310F0, 16, 16, TF_IA8);   /* gFileSelRupeeTex */
    for (k = 0; k < 4; k++) ArtLoad(ArtNew(A_REMAINS0 + k, 32, 32, 1), 0, 0, ROM_TITLE, kRemains[k], 32, 32, TF_RGBA32);
    ArtLoad(ArtNew(A_SOCKET, 32, 32, 0), 0, 0, ROM_PARAM, 0xF60, 32, 32, TF_IA8);     /* gButtonBackgroundTex */
    ArtLoad(ArtNew(A_METER_END, 8, 16, 0), 0, 0, ROM_PARAM, 0x4DA0, 8, 16, TF_IA8);   /* gMagicMeterEndTex */
    ArtLoad(ArtNew(A_METER_MID, 24, 16, 0), 0, 0, ROM_PARAM, 0x4E20, 24, 16, TF_IA8); /* gMagicMeterMidTex */
    ArtLoad(ArtNew(A_KEY, 16, 16, 0), 0, 0, ROM_PARAM, 0xD60, 16, 16, TF_IA8);        /* gSmallKeyCounterIconTex */
    a = ArtNew(A_DIGITS, 80, 8, 0);
    for (k = 0; k < 10; k++) ArtLoad(a, k * 8, 0, ROM_PARAM, 0x4AA0 + k * 0x40, 8, 8, TF_IA8); /* gAmmoDigit0Tex .. 9 */
    { /* PORT (2026-11-08): the three-day clock's hour digits (gThreeDayClockHour1Tex .. 9Tex) as a font, and its 0 from
       * gThreeDayClockHour10Tex (no hour shows a 0 alone); the sun and the moon that mark the hours */
        static u8 sTen[16 * 11 * 2];
        PArt ten = { 16, 11, 0, sTen };
        int x, y, x0 = -1, x1 = -1, any[16], e1 = -1;
        a = ArtNew(A_CLKDIG, 160, 11, 0);
        for (k = 1; k <= 9; k++) ArtLoad(a, k * 16, 0, ROM_PARAM, 0x6498 + (k - 1) * 0x58, 16, 11, TF_I4);
        memset(sTen, 0, sizeof(sTen));
        ArtLoad(&ten, 0, 0, ROM_PARAM, 0x67B0, 16, 11, TF_I4);
        for (x = 0; x < 16; x++) {
            any[x] = 0;
            for (y = 0; y < 11; y++) any[x] |= sTen[(y * 16 + x) * 2 + 1] != 0;
        }
        for (x = 0; x < 16 && !any[x]; x++) {} /* the "1" of "10" */
        for (; x < 16 && any[x]; x++) e1 = x;
        for (; x < 16; x++) { /* the "0": what comes after it */
            if (!any[x]) continue;
            if (x0 < 0) x0 = x;
            x1 = x;
        }
        (void)e1;
        if (x0 < 0) x0 = 8, x1 = 15; /* (no gap found: its right half) */
        if (a->px != NULL && x0 >= 0) {
            for (y = 0; y < 11; y++)
                for (x = x0; x <= x1; x++) memcpy(a->px + (y * 160 + x) * 2, sTen + (y * 16 + x) * 2, 2);
        }
        if (a->px != NULL) { /* the colour is the primitive's alone (the game's combiner): full intensity, the texel's alpha */
            for (k = 0; k < 160 * 11; k++) a->px[k * 2] = 255;
        }
        ArtLoad(ArtNew(A_CLKSUN, 24, 24, 0), 0, 0, ROM_PARAM, 0x5720, 24, 24, TF_IA8);  /* gThreeDayClockSunHourTex */
        ArtLoad(ArtNew(A_CLKMOON, 24, 24, 0), 0, 0, ROM_PARAM, 0x54E0, 24, 24, TF_IA8); /* gThreeDayClockMoonHourTex */
    }
    a = ArtNew(A_COUNTER, 80, 16, 0);
    for (k = 0; k < 10; k++) ArtLoad(a, k * 8, 0, ROM_PARAM, 0x44A0 + k * 0x80, 8, 16, TF_I8); /* gCounterDigit0Tex .. 9 */
    for (k = 0; k < 5; k++) { /* the hearts as the HUD draws them: environment (50, 40, 60) to primitive (255, 70, 50) */
        u8 ia[16 * 16 * 2], rgba[16 * 16 * 4];
        PArt t = { 16, 16, 0, ia };
        int p;
        memset(ia, 0, sizeof(ia));
        ArtLoad(&t, 0, 0, ROM_PARAM, kHearts[k], 16, 16, TF_IA8);
        {
            PArt* hia = ArtNew(A_HEARTIA0 + k, 16, 16, 0);
            if (hia->px != NULL) memcpy(hia->px, ia, sizeof(ia));
        }
        for (p = 0; p < 16 * 16; p++) {
            int I = ia[2 * p];
            rgba[4 * p] = (u8)(50 + 205 * I / 255), rgba[4 * p + 1] = (u8)(40 + 30 * I / 255);
            rgba[4 * p + 2] = (u8)(60 - 10 * I / 255), rgba[4 * p + 3] = ia[2 * p + 1];
        }
        ArtShrink(rgba, 16, 16, ArtNew(A_HEART0 + k, 10, 10, 1));
    }
    for (k = 0; k < 5; k++) { /* PORT (2026-11-08): the double defense hearts (gDefenseHeartEmptyTex .. FullTex), as the HUD
                               * draws them: primitive (255, 255, 255) to environment (200, 0, 0) - a white rim, red */
        u8 ia[16 * 16 * 2], rgba[16 * 16 * 4];
        PArt t = { 16, 16, 0, ia };
        int p;
        memset(ia, 0, sizeof(ia));
        ArtLoad(&t, 0, 0, ROM_PARAM, 0x500 + k * 0x100, 16, 16, TF_IA8);
        {
            PArt* hia = ArtNew(A_DHEARTIA0 + k, 16, 16, 0);
            if (hia->px != NULL) memcpy(hia->px, ia, sizeof(ia));
        }
        for (p = 0; p < 16 * 16; p++) {
            int I = ia[2 * p];
            rgba[4 * p] = (u8)(255 - 55 * I / 255), rgba[4 * p + 1] = (u8)(255 - 255 * I / 255);
            rgba[4 * p + 2] = (u8)(255 - 255 * I / 255), rgba[4 * p + 3] = ia[2 * p + 1];
        }
        ArtShrink(rgba, 16, 16, ArtNew(A_DHEART0 + k, 10, 10, 1));
    }
    ArtMoon();
    sArtState = 1;
}
static void ArtReady(void) {
    if (!sArtState) ArtLoadAll();
}

/* an art drawn into (x, y, w, h), its borders (l, r, t, b pixels) kept and the middle stretched (a nine-slice: the
 * file select's button as the side buttons and the settings rows); IA art: colour = intensity x tint; alpha x amul/256 */
static void ArtDraw9(const PArt* a, int x, int y, int w, int h, int l, int r, int t, int b, u32 tint, int amul,
                     int flipX) {
    int i, j, tr = tint >> 16, tg = (tint >> 8) & 255, tb = tint & 255;
    if (a->px == NULL || w <= 0 || h <= 0) return;
    if (l + r >= w || l + r >= a->w) l = r = 0;
    if (t + b >= h || t + b >= a->h) t = b = 0;
    /* (only the part inside the clip: a part of the window is drawn again under every cell) */
    for (j = sClipY0 - y > 0 ? sClipY0 - y : 0; j < h && y + j < sClipY1; j++) {
        int sy = h == a->h ? j : j < t ? j : j >= h - b ? a->h - (h - j) : t + (j - t) * (a->h - t - b) / (h - t - b);
        for (i = sClipX0 - x > 0 ? sClipX0 - x : 0; i < w && x + i < sClipX1; i++) {
            int sx = w == a->w ? i : i < l ? i : i >= w - r ? a->w - (w - i) : l + (i - l) * (a->w - l - r) / (w - l - r);
            const u8* s;
            if (flipX) sx = a->w - 1 - sx;
            if (a->rgba) {
                s = a->px + (sy * a->w + sx) * 4;
                if (s[3] == 0) continue;
                PxA(x + i, y + j, Div255(s[0] * tr), Div255(s[1] * tg), Div255(s[2] * tb), s[3] * amul >> 8);
            } else {
                s = a->px + (sy * a->w + sx) * 2;
                if (s[1] == 0) continue;
                PxA(x + i, y + j, Div255(s[0] * tr), Div255(s[0] * tg), Div255(s[0] * tb), s[1] * amul >> 8);
            }
        }
    }
}
static void ArtDraw(const PArt* a, int x, int y, u32 tint, int amul) {
    ArtDraw9(a, x, y, a->w, a->h, 0, 0, 0, 0, tint, amul, 0);
}
/* the file select's blank button stretched to w x h */
static void ButtonFace(int x, int y, int w, int h, u32 tint) {
    ArtDraw9(&sArt[A_BLANK], x, y, w, h, 7, 5, 4, 3, tint, 256, 0);
}
/* a small RGBA icon (the design's vector icons) */
static void IconDraw(const u8* rgba, int x, int y, int w, int h, u32 tint, int amul) {
    PArt a;
    a.w = w, a.h = h, a.rgba = 1, a.px = (u8*)rgba;
    ArtDraw(&a, x, y, tint, amul);
}

/* an ammo count as the HUD draws it (Interface_DrawAmmoCount): its own digits, 6 pixels apart; green at the most it can
 * hold, grey when empty, white otherwise. x is the left end, or the right end with right set */
static void DrawAmmo(int x, int y, int ammo, int full, int right) {
    char buf[8];
    int n, k;
    u32 col = ammo == 0 ? RGB24(100, 100, 100) : full ? RGB24(120, 255, 0) : COL_WHITE;
    n = snprintf(buf, sizeof(buf), "%d", ammo < 0 ? 0 : ammo);
    if (right) x -= 8 + 6 * (n - 1);
    for (k = 0; k < n; k++) {
        ClipSet(x + k * 6, y, 8, 8);
        ArtDraw(&sArt[A_DIGITS], x + k * 6 - (buf[k] - '0') * 8, y, col, 256);
    }
    ClipReset();
}

/* the background (and the window, unless the settings page is up) under (x, y, w, h): a part is drawn again from it */
static int sSettingsOpen; /* the settings page fills the centre column (PanelSettings_*) */
static void BackLayer(int x, int y, int w, int h) {
    ClipSet(x, y, w, h);
    FillRect(x, y, w, h, ColBg());
    if (!sSettingsOpen) ArtDraw(&sArt[A_WINDOW], WIN_X, WIN_Y, ThemeTint(), 256);
    ClipReset();
}

/* 32x32 RGBA32 item icon at size px (area average), alpha-blended; dim = disabled button */
static void DrawIcon(int x, int y, const u8* rgba, int size, int dim) {
    int i, j, x0 = 32, x1 = -1, y0 = 32, y1 = -1;
    if (rgba == NULL) return;
    /* centered on the pixels the art covers, not on its 32x32 cell: item art sits up to 2 texels off-center
     * (Kokiri boots 2 right, bomb 1 right: visibly off on the plates, 2026-10-03) */
    for (j = 0; j < 32; j++) {
        for (i = 0; i < 32; i++) {
            if (rgba[(((j * 32 + i) * 4) + 3)] != 0) {
                x0 = i < x0 ? i : x0, x1 = i > x1 ? i : x1;
                y0 = j < y0 ? j : y0, y1 = j > y1 ? j : y1;
            }
        }
    }
    if (x1 < 0) return;
    x += RoundDiv((32 - (x0 + x1 + 1)) * size, 64);
    y += RoundDiv((32 - (y0 + y1 + 1)) * size, 64);
    for (j = 0; j < size; j++) {
        for (i = 0; i < size; i++) {
            /* the texels under this pixel, averaged (nearest left 32 -> 24 blocky) */
            int sx0 = i * 32 / size, sx1 = ((i + 1) * 32 + size - 1) / size, sy0 = j * 32 / size,
                sy1 = ((j + 1) * 32 + size - 1) / size, sx, sy, r = 0, g = 0, b = 0, a = 0, n = 0;
            for (sy = sy0; sy < sy1; sy++) {
                for (sx = sx0; sx < sx1; sx++) {
                    const u8* s = rgba + (sy * 32 + sx) * 4;
                    r += s[0] * s[3], g += s[1] * s[3], b += s[2] * s[3], a += s[3], n++;
                }
            }
            if (a == 0) continue;
            r /= a, g /= a, b /= a, a /= n;
            if (dim) r = g = b = (r + g + b) / 6;
            PxA(x + i, y + j, r, g, b, a);
        }
    }
}


/* ---- panel state (what is on screen) ---- */
static int sHeldPad = -1;
static PortHudInfo sShown;
static int sShownIconsOk = -1, sShownTatl;
static int sDimmed;       /* the panel is shown dimmed: a cutscene is playing (PanelDimWanted) */
static int sPausePanel;   /* the pause panel (SAVE) is shown in the centre window (paused, no settings page) */
static int sTatlPulse;
static const u8* sShownIconSeg;
static unsigned sLastHudSerial, sLastMapSerial;
static int sHudStale = 99, sMapStale = 99;

static int IconsOk(void) {
    return sHudStale < 4 && gPortHudIconSeg != NULL;
}

/* the final hours (the third night past midnight): "Ocarina!" */
static int FinalHours(void) {
    return sShown.valid && sShown.day == 3 && sShown.minutes < 6 * 60;
}
static int sDragHover = -1; /* the button an item is being dragged over (drawn lit) */

/* the design's cells (and the minimap's frame): the window darkened, a light hairline */
static u32 CellBgRGB(void) {
    return sThemeRed ? RGB24(36, 6, 10) : RGB24(6, 10, 36);
}
static u32 CellLineRGB(void) {
    return sThemeRed ? RGB24(255, 180, 160) : RGB24(170, 196, 255);
}

/* a socket's letter: a light badge at its lower right (the design's X / Y / I / II) */
static void SocketBadge(int sx, int sy, const char* s) {
    int w = PTextW(F_8B, s, 0) + 8, bx = sx + 34 - w, by = sy + 22;
    FillRect(bx, by, w, 12, PRGB(0x10, 0x18, 0x3a));
    FillRect(bx + 1, by + 1, w - 2, 10, PRGB(0xe8, 0xee, 0xff));
    PTextMid(bx + w / 2, by + 1, 10, s, F_8B, COL_INK, 0);
}

/* Tatl calling: a pulsing fairy on the View button */
static void DrawFairy(int cx, int fy, int green) {
    int dx, dy, gr = 255, gg = green ? 235 : 210, gb = green ? 120 : 160;
    for (dy = -12; dy <= 12; dy++) {
        for (dx = -16; dx <= 16; dx++) {
            int sx = dx * 5 / 4, sy = dy * 5 / 4, d2 = sx * sx + sy * sy;
            int wx = sx < 0 ? -sx : sx, wyU = sy + 5, wyD = sy - 7;
            if ((wx > 5 && (wx - 13) * (wx - 13) * 9 + wyU * wyU * 49 <= 9 * 49 * 4) ||
                (wx > 5 && (wx - 11) * (wx - 11) * 16 + wyD * wyD * 64 <= 16 * 64 * 2)) {
                PxA(cx + dx, fy + dy, 200, 235, 255, 150);
            }
            if (d2 <= 144) PxA(cx + dx, fy + dy, gr, gg, gb, 255 - d2 * 255 / 144);
            if (d2 <= 16) PxA(cx + dx, fy + dy, 250, 255, 255, 255);
        }
    }
}

static void DrawPad(int i) {
    const PanelPad* p = &sPads[i];
    int pressed = i == sHeldPad || i == sDragHover;
    if (p->page >= 0) return; /* the tabs: drawn with the window (DrawTabs) */
    if (i >= P_CLEFT && i <= P_MASKS) { /* the sockets: the HUD's C-button background, the item, the letter */
        int sx = 284, sy = p->y;
        BackLayer(281, sy, 39, i == P_MASKS ? 240 - sy : 36);
        ArtDraw(&sArt[A_SOCKET], sx, sy, RGB24(255, 240, 0), pressed ? 256 : 153);
        if (i == sDragHover) { /* an item dragged over it: a light ring */
            int dx, dy;
            for (dy = -17; dy <= 17; dy++) {
                for (dx = -17; dx <= 17; dx++) {
                    float d = sqrtf((dx + 0.5f) * (dx + 0.5f) + (dy + 0.5f) * (dy + 0.5f));
                    float a = 1.0f - fabsf(d - 15.0f); /* (inside the socket's cell: 16 px around its centre) */
                    if (a > 0.0f) PxA(sx + 16 + dx, sy + 16 + dy, 255, 250, 220, (int)(a * 255));
                }
            }
        }
        if (i == P_MASKS) {
            if (sShown.valid && sShown.mask != 0xFF) DrawIcon(sx + 4, sy + 4, Port_GetItemIcon(sShown.mask), 24, 0);
        } else {
            int c = i - P_CLEFT;
            if (sShown.cItem[c] < 0xF0 && IconsOk()) {
                DrawIcon(sx + 4, sy + 4, gPortHudIconSeg + (c + 1) * 32 * 32 * 4, 24, sShown.cDisabled[c]);
                if (sShown.cAmmo[c] >= 0) DrawAmmo(sx, sy + 24, sShown.cAmmo[c], sShown.cAmmoFull[c], 0);
            }
        }
        SocketBadge(sx, sy, p->label);
        sFbDirty = 1;
        return;
    }
    /* View, Setup, Ocarina: the file select's button, an icon, the name */
    {
        int cx = p->x + p->w / 2, have = sShown.valid && sShown.ocarina == PANEL_ITEM_OCARINA_OF_TIME;
        int final = i == P_OCARINA && FinalHours() && have;
        u32 label = COL_WHITE;
        const char* name = p->label;
        BackLayer(0, p->y, WIN_X, p->h);
        ButtonFace(p->x, p->y, p->w, p->h, pressed ? ThemeTintHi() : ThemeTint());
        if (final) { /* "Ocarina!": a warm frame (the design's final hours) */
            FrameA(p->x, p->y, p->w, p->h, RGB24(0xff, 0xd0, 0xa0), 255);
            FrameA(p->x + 1, p->y + 1, p->w - 2, p->h - 2, RGB24(0xff, 0xd0, 0xa0), 128);
        }
        if (i == P_OCARINA) {
            IconDraw(kIcon_ocarina, cx - 10, p->y + 11, 20, 14, COL_WHITE, have ? 256 : 96);
            name = final ? "Ocarina!" : "Ocarina";
            label = final ? RGB24(0xff, 0xe0, 0xb0) : have ? COL_WHITE : COL_OFF;
        } else if (i == P_VIEW && sShownTatl) {
            DrawFairy(cx, p->y + 17, sTatlPulse);
            name = "Tatl";
            label = sTatlPulse ? RGB24(255, 235, 130) : COL_WHITE;
        } else if (i == P_VIEW) {
            IconDraw(kIcon_eye, cx - 10, p->y + 11, 20, 14, COL_WHITE, 256);
        } else if (i == P_SETUP) {
            IconDraw(kIcon_gear, cx - 7, p->y + 10, 15, 15, sSettingsOpen ? COL_GOLD24 : COL_WHITE, 256);
            name = sSettingsOpen ? "Back" : "Setup";
        }
        {   /* (squeezed a little if wider than the button: "Ocarina!") */
            int ls8 = PTextW(F_8B, name, 0) > p->w - 6 ? -4 : 0;
            PTextB(cx - PTextW8(F_8B, name, ls8) / 2, p->y + 28 + kPFonts[F_8B].asc, name, F_8B, label, 1, ls8);
        }
    }
    sFbDirty = 1;
}

/* ---- the hearts (PORT 2026-11-07: with the HUD's beating heart, user request) ----
 * As z_lifemeter.c LifeMeter_Draw: the last heart with anything in it beats - drawn 1 / 0.68 of a heart's size
 * shrinking back to it as lifeSizeChange runs 0..10 (16 px down to 10.9 on the HUD; here 14.7 down to 10), in the beat
 * colours (which pulse too when health is low); the hearts after it are drawn over it, as there. */
static int HeartState(int i) { /* 0 empty, 1..3 quarters, 4 full */
    int f = sShown.health - i * 16, st;
    f = f < 0 ? 0 : (f > 16 ? 16 : f);
    st = f >= 16 ? 4 : (f + 3) / 4; /* the HUD's quarters */
    return st > 3 && f < 16 ? 3 : st;
}
static int BeatingHeart(void) { /* its index, -1 none */
    return sShown.health <= 0 ? -1 : sShown.health / 16 - (sShown.health % 16 == 0);
}
static void DrawBeatingHeart(int st, float cx, float cy, float size, int dd) {
    /* a double defense heart: from primitive (255, 255, 255) to environment (200, 0, 0), which don't beat in colour
     * (z_lifemeter.c sBeatingHeartsDDPrim / Env with the factors of type 0) */
    static const u8 kDDPrim[3] = { 255, 255, 255 }, kDDEnv[3] = { 200, 0, 0 };
    const u8* lo = dd ? kDDPrim : sShown.beatEnv; /* (at intensity 0) */
    const u8* hi = dd ? kDDEnv : sShown.beatPrim; /* (at 255) */
    const PArt* a = &sArt[(dd ? A_DHEARTIA0 : A_HEARTIA0) + st];
    int x0 = (int)floorf(cx - size / 2), y0 = (int)floorf(cy - size / 2), x1 = (int)ceilf(cx + size / 2),
        y1 = (int)ceilf(cy + size / 2), x, y, sx, sy, k;
    float inv = 16.0f / size;
    if (a->px == NULL) return;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            int acc[3] = { 0, 0, 0 }, aa = 0;
            for (sy = 0; sy < 3; sy++) { /* 3 x 3 samples per pixel */
                float v = ((float)y + (sy + 0.5f) / 3.0f - (cy - size / 2)) * inv;
                if (v < 0.0f || v >= 16.0f) continue;
                for (sx = 0; sx < 3; sx++) {
                    float u = ((float)x + (sx + 0.5f) / 3.0f - (cx - size / 2)) * inv;
                    const u8* t;
                    if (u < 0.0f || u >= 16.0f) continue;
                    t = a->px + ((int)v * 16 + (int)u) * 2;
                    for (k = 0; k < 3; k++) {
                        int c = lo[k] + (hi[k] - lo[k]) * t[0] / 255;
                        acc[k] += c * t[1];
                    }
                    aa += t[1];
                }
            }
            if (aa == 0) continue;
            PxA(x, y, acc[0] / aa, acc[1] / aa, acc[2] / aa, aa / 9);
        }
    }
}
static void DrawHeartRows(void) {
    int hearts = sShown.healthCapacity / 16, beat = BeatingHeart(), i;
    for (i = 0; i < hearts && i < 20; i++) {
        int x = 11 + (i % 10) * 10, y = 21 + (i / 10) * 10;
        int dd = i < sShown.defHearts; /* (z_lifemeter.c: the first defenseHearts hearts) */
        if (i == beat) {
            DrawBeatingHeart(HeartState(i), x + 5.0f, y + 5.0f, 10.0f * (1.0f - 0.032f * sShown.lifeSize) / 0.68f, dd);
        } else {
            ArtDraw(&sArt[(dd ? A_DHEART0 : A_HEART0) + HeartState(i)], x, y, COL_WHITE, 256);
        }
    }
}
/* the hearts' area alone, drawn again (each beat): its background, the file-info box under it, the hearts */
#define HEARTS_X 4
#define HEARTS_Y 17 /* (below the name's descenders) */
#define HEARTS_W 112
#define HEARTS_H 26
static int sDirtyX0 = 320, sDirtyX1; /* columns changed outside a whole update (copied alone: Port3ds_TouchUiPoll) */
static void DrawMagicMeter(void);
static void RedrawHearts(void) {
    ClipSet(HEARTS_X, HEARTS_Y, HEARTS_W, HEARTS_H);
    FillRect(HEARTS_X, HEARTS_Y, HEARTS_W, HEARTS_H, ColBg());
    ArtDraw(&sArt[A_INFO], 2, 2, ThemeTint(), 256);
    DrawHeartRows();
    DrawMagicMeter(); /* (its top runs under the hearts' area with one row of them) */
    ClipReset();
    sDirtyX0 = sDirtyX0 < HEARTS_X ? sDirtyX0 : HEARTS_X;
    sDirtyX1 = sDirtyX1 > HEARTS_X + HEARTS_W ? sDirtyX1 : HEARTS_X + HEARTS_W;
}

/* the magic meter: the HUD's end, middle (repeated over the capacity) and mirrored end; the fill */
static void DrawMagic(int x, int y) {
    int cap = sShown.magicCapacity, fill = sShown.magic, k;
    int c0 = sClipX0, c1 = sClipY0, c2 = sClipX1, c3 = sClipY1; /* (drawn inside whatever clip is set) */
    if (cap > 104) cap = 104;
    ArtDraw(&sArt[A_METER_END], x, y, COL_WHITE, 256);
    for (k = 0; k < cap; k += 24) { /* the middle repeated over the capacity, cut at its end */
        sClipX0 = c0 > x + 8 ? c0 : x + 8, sClipX1 = c2 < x + 8 + cap ? c2 : x + 8 + cap;
        ArtDraw(&sArt[A_METER_MID], x + 8 + k, y, COL_WHITE, 256);
    }
    sClipX0 = c0, sClipY0 = c1, sClipX1 = c2, sClipY1 = c3;
    ArtDraw9(&sArt[A_METER_END], x + 8 + cap, y, 8, 16, 0, 0, 0, 0, COL_WHITE, 256, 1);
    if (fill > cap) fill = cap;
    if (fill > 0) FillRectA(x + 8, y + 3, fill, 7, RGB24(0, 200, 0), 255);
}
/* PORT (2026-11-07, user request: spaced exactly like the top screen): where Magic_DrawMeter puts it relative to the
 * hearts - x 18 against the first heart's centre at 30, y 42 under two rows of hearts or 34 under one - with the hearts
 * here centred at x 16 (11 + 5) and y 26 like there: x 4, y 42 / 34 */
static void DrawMagicMeter(void) {
    if (sShown.valid && sShown.magicCapacity > 0) DrawMagic(4, sShown.healthCapacity > 0xA0 ? 42 : 34);
}

/* the rupees as the HUD draws them (Interface_Draw): the counter digits 8 pixels apart, two with the first wallet and three
 * with the others, a dark copy a pixel down and right under them; green with the wallet full, grey at none, white
 * otherwise */
static void DrawRupees(int x, int y) {
    static const u8 kFirst[3] = { 1, 0, 0 }, kCount[3] = { 2, 3, 3 };
    int w = sShown.wallet > 2 ? 2 : sShown.wallet, v = sShown.rupees, d[3], k, pass;
    int cx0 = sClipX0, cy0 = sClipY0, cx1 = sClipX1, cy1 = sClipY1;
    u32 col = sShown.rupeesFull ? RGB24(120, 255, 0) : v != 0 ? COL_WHITE : RGB24(100, 100, 100);
    if (v > 9999 || v < 0) v &= 0xDDD;
    d[0] = v / 100 % 10, d[1] = v / 10 % 10, d[2] = v % 10;
    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < kCount[w]; k++) {
            int dx = x + k * 8 + (pass == 0), dy = y + (pass == 0);
            sClipX0 = dx < cx0 ? cx0 : dx, sClipY0 = dy < cy0 ? cy0 : dy;
            sClipX1 = dx + 8 > cx1 ? cx1 : dx + 8, sClipY1 = dy + 16 > cy1 ? cy1 : dy + 16;
            ArtDraw(&sArt[A_COUNTER], dx - d[kFirst[w] + k] * 8, dy, pass == 0 ? RGB24(0, 0, 0) : col, pass == 0 ? 181 : 256);
        }
    }
    sClipX0 = cx0, sClipY0 = cy0, sClipX1 = cx1, sClipY1 = cy1;
}

/* PORT (2026-11-08): the time in the three-day clock's own digits (A_CLKDIG), twice their size, with the dark copy a
 * pixel down and right that the clock draws under them (z_parameter.c Interface_DrawClock); x is the right end. Returns
 * the width. */
static int ClockGlyph(int d, int* x0) { /* a digit's columns in its cell */
    const PArt* a = &sArt[A_CLKDIG];
    int x, y, lo = -1, hi = -1;
    if (a->px == NULL) return 0;
    for (x = 0; x < 16; x++) {
        int any = 0;
        for (y = 0; y < 11; y++) any |= a->px[(y * 160 + d * 16 + x) * 2 + 1] != 0;
        if (any) {
            if (lo < 0) lo = x;
            hi = x;
        }
    }
    *x0 = lo < 0 ? 0 : lo;
    return lo < 0 ? 6 : hi - lo + 1;
}
static int DrawClockTime(int xr, int y, const char* s, u32 col) {
    int pass, w = 0, i, x0;
    for (i = 0; s[i]; i++) w += (s[i] == ':' ? 2 : ClockGlyph(s[i] - '0', &x0)) * 2 + (s[i + 1] ? 2 : 0);
    for (pass = 0; pass < 2; pass++) {
        int x = xr - w + (pass == 0 ? 2 : 0), yy = y + (pass == 0 ? 2 : 0);
        u32 c = pass == 0 ? RGB24(0, 0, 0) : col;
        for (i = 0; s[i]; i++) {
            if (s[i] == ':') {
                FillRectA(x, yy + 6, 4, 4, c, 255);
                FillRectA(x, yy + 14, 4, 4, c, 255);
                x += 4 + 2;
                continue;
            }
            {
                int d = s[i] - '0', gw = ClockGlyph(d, &x0);
                int cx0 = sClipX0, cy0 = sClipY0, cx1 = sClipX1, cy1 = sClipY1;
                sClipX0 = x > cx0 ? x : cx0, sClipY0 = yy > cy0 ? yy : cy0;
                sClipX1 = x + gw * 2 < cx1 ? x + gw * 2 : cx1, sClipY1 = yy + 22 < cy1 ? yy + 22 : cy1;
                ArtDraw9(&sArt[A_CLKDIG], x - (d * 16 + x0) * 2, yy, 320, 22, 0, 0, 0, 0, c, pass == 0 ? 220 : 256, 0);
                sClipX0 = cx0, sClipY0 = cy0, sClipX1 = cx1, sClipY1 = cy1;
                x += gw * 2 + 2;
            }
        }
    }
    return w;
}

/* the header: the file-info box (rupees, small keys, the name, hearts, magic), the day box, the clock, the three days */
static void DrawHeader(void) {
    char buf[24];
    int red = sThemeRed, valid = sShown.valid && sShown.day >= 1 && sShown.day <= 3;
    int elapsed = 0;
    if (sSettingsOpen) return; /* the settings page covers the centre (the header is not drawn) */
    if (valid) elapsed = (sShown.day - 1) * 1440 + ((sShown.minutes - 360 + 1440) % 1440); /* since 6:00, 1st day */
    ClipSet(0, 0, 320, WIN_Y);
    FillRect(0, 0, 320, WIN_Y, ColBg());
    /* the file-info box */
    ArtDraw(&sArt[A_INFO], 2, 2, ThemeTint(), 256);
    if (sShown.rupees >= 0) {
        static const u32 kWalletCol[3] = { RGB24(200, 255, 100), RGB24(170, 170, 255), RGB24(255, 105, 105) };
        ArtDraw(&sArt[A_RUPEE], 5, 1, kWalletCol[sShown.wallet > 2 ? 2 : sShown.wallet], 256);
        DrawRupees(23, 1);
    }
    if (sShown.keys >= 0) { /* small keys: under the hearts, right of the magic meter */
        ArtDraw(&sArt[A_KEY], 140, 43, RGB24(200, 230, 255), 256);
        snprintf(buf, sizeof(buf), "%d", sShown.keys);
        PText(156, 44, buf, F_12B, COL_WHITE, 1, 0);
    }
    if (sShown.valid && sShown.name[0]) PText(84, 4, sShown.name, F_10S, COL_WHITE, 1, 3);
    if (sShown.valid) {
        DrawHeartRows();
        DrawMagicMeter();
    }
    /* the day box: the hours left, the day */
    ArtDraw(&sArt[A_DAYBOX], 172, 2, ThemeTint(), 256);
    if (valid) {
        int left = 4320 - elapsed;
        if (left < 0) left = 0;
        if (left >= 6 * 60) snprintf(buf, sizeof(buf), "%dh left", left / 60);
        else snprintf(buf, sizeof(buf), "%d:%02d left", left / 60, left % 60);
        PTextC(200, 4, buf, red ? F_8B : F_8S, red ? RGB24(0xff, 0xd0, 0xc4) : COL_WHITE, 1);
        ArtDraw(&sArt[A_DAY1 + sShown.day - 1], 176, 27, COL_WHITE, 256);
    }
    /* the clock: the sun or the moon, the time, AM / PM */
    if (valid) {
        int min = sShown.minutes, h12 = (min / 60) % 12, night = min < 6 * 60 || min >= 18 * 60, tw;
        const char* ap = min < 12 * 60 ? "AM" : "PM";
        snprintf(buf, sizeof(buf), "%d:%02d", h12 == 0 ? 12 : h12, min % 60);
        /* PORT (2026-11-08): the top screen clock's digits (pale yellow there) and its sun / moon (their tints) */
        DrawClockTime(316, 6, buf, red ? RGB24(0xff, 0x9a, 0x85) : RGB24(255, 255, 155));
        tw = PTextW(F_11B, ap, 0);
        PTextB(316 - tw, 48, ap, F_11B, red ? RGB24(0xff, 0xb4, 0xa4) : COL_GOLD24, 1, 0);
        ArtDraw(&sArt[night ? A_CLKMOON : A_CLKSUN], 316 - tw - 3 - 24, 31,
                night ? RGB24(255, 255, 55) : RGB24(255, 100, 110), 256);
    }
    /* the three days: day and night halves, what has passed darker; Link's place; the moon at the end */
    {
        int bx = 5, by = 66, bw = red ? 286 : 292, k;
        FrameA(bx - 1, by - 1, bw + 2, 7, red ? RGB24(0xff, 0x8a, 0x6a) : RGB24(0x9a, 0xb8, 0xff), 255);
        for (k = 0; k < bw; k++) {
            int t = k * 4320 / bw, night = (t % 1440) >= 720, past = valid && t < elapsed;
            u16 c;
            if (red) c = past ? (night ? PRGB(0x2e, 0x1c, 0x26) : PRGB(0x4a, 0x30, 0x38)) : PRGB(0xe0, 0x4a, 0x36);
            else c = past ? (night ? PRGB(0x26, 0x2c, 0x48) : PRGB(0x3c, 0x44, 0x66))
                          : (night ? PRGB(0x1c, 0x2a, 0x66) : PRGB(0x6b, 0x8f, 0xe0));
            FillRect(bx + k, by, 1, 5, c);
        }
        FillRect(bx + bw / 3 - 1, by, 1, 5, ColBg()); /* between the days */
        FillRect(bx + bw * 2 / 3 - 1, by, 1, 5, ColBg());
        if (valid) {
            int mx = bx + elapsed * bw / 4320, r;
            for (r = 0; r < 4; r++) FillRect(mx - 3 + r, 60 + r, 7 - 2 * r, 1, red ? PRGB(0xff, 0x9a, 0x85) : PRGB(255, 255, 255));
        }
        if (red) { /* the moon, far too close: larger, in a red ring */
            int dx, dy;
            for (dy = 0; dy < 20; dy++) {
                for (dx = 0; dx < 20; dx++) {
                    float d = sqrtf((dx - 9.5f) * (dx - 9.5f) + (dy - 9.5f) * (dy - 9.5f)), a = 1.0f - fabsf(d - 9.3f) / 1.1f;
                    if (a > 0.0f) PxA(297 + dx, 58 + dy, 0xe0, 0x4a, 0x36, (int)(a * 255));
                }
            }
            ArtDraw(&sArt[A_MOON_BIG], 298, 59, COL_WHITE, 256);
        } else {
            ArtDraw(&sArt[A_MOON], 300, 60, COL_WHITE, 256);
        }
    }
    ClipReset();
    sFbDirty = 1;
}

/* ---- minimap ---- */
static void MapTri(float cx, float cy, short yaw, float size, u16 col) {
    /* arrow along yaw: world +x is right and +z is down on the map */
    float a = yaw * (3.14159265f / 32768.0f);
    float fx = sinf(a), fy = cosf(a);
    float px[3] = { cx + fx * size, cx - fx * size * 0.6f + fy * size * 0.6f, cx - fx * size * 0.6f - fy * size * 0.6f };
    float py[3] = { cy + fy * size, cy - fy * size * 0.6f - fx * size * 0.6f, cy - fy * size * 0.6f + fx * size * 0.6f };
    int x0 = (int)floorf(fminf(px[0], fminf(px[1], px[2]))), x1 = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
    int y0 = (int)floorf(fminf(py[0], fminf(py[1], py[2]))), y1 = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
    int x, y, k;
    for (y = y0 - 1; y <= y1 + 1; y++) {
        for (x = x0 - 1; x <= x1 + 1; x++) {
            int pos = 0, neg = 0;
            for (k = 0; k < 3; k++) {
                int n = (k + 1) % 3;
                float e = (px[n] - px[k]) * (y + 0.5f - py[k]) - (py[n] - py[k]) * (x + 0.5f - px[k]);
                if (e >= 0) pos++; else neg++;
            }
            if ((pos == 3 || neg == 3) && x >= MAP_X && x < MAP_X + MAP_W && y >= MAP_Y && y < MAP_Y + MAP_H) {
                Px(x, y, col);
            }
        }
    }
}

static int MapTexel(const PortMinimapLayer* m, int tx, int ty, int* lum) {
    const u8* t = m->tex;
    if (m->flipX) tx = m->w - 1 - tx;
    if (m->flipY) ty = m->h - 1 - ty;
    int idx = ty * m->w + tx;
    int nib = (t[(idx >> 1) ^ m->byteXor] >> ((idx & 1) ? 0 : 4)) & 0xF;
    if (m->fmt != PORT_MINIMAP_IA4) {
        *lum = m->fmt == PORT_MINIMAP_I4_ALPHA ? 255 : nib * 17;
        return nib * 17 * m->a / 255;
    }
    *lum = (nib >> 1) * 255 / 7;
    return (nib & 1) ? m->a : 0;
}

static void DrawMapInto(int have);

/* The bottom framebuffer is single-buffered and scanned out while we draw: repainting the parchment and
 * then the map in place showed the half-drawn state (the map flashed on hardware). Draw into a shadow
 * copy with the framebuffer's layout, then copy only the map columns over in one pass. */
extern void Port3ds_CacheFlush(const void* p, u32 size); /* gfx_3ds.c */
static int sFbMapDirty; /* only the map's columns changed: the flush covers just those (PORT PERF 2026-10-04) */
static int sMapLastHave;  /* what the last DrawMap showed (a dimmed panel's redraw shows it again) */
static int sMapForce;     /* after a whole-panel redraw: draw the minimap again even if it did not change */
static void DrawMap(int have) {
    static u16 sShadow[320 * 240];
    u16* real = sFb;
    int x, dirty = sFbDirty;
    sMapLastHave = have;
    if (sSettingsOpen || sPausePanel || sPage != PG_MAP) return; /* the settings page / pause panel / another page */
    sFb = sShadow;
    DrawMapInto(have);
    sFb = real;
    sFbDirty = dirty; /* (the shadow copy is not scanned out) */
    for (x = MAP_X; x < MAP_X + MAP_W; x++) {
        /* column x holds rows 239..0; the map's rows MAP_Y..MAP_Y+MAP_H-1 are contiguous in it */
        int base = x * 240 + (239 - (MAP_Y + MAP_H - 1));
        memcpy(&real[base], &sShadow[base], MAP_H * sizeof(u16));
    }
    sFbMapDirty = 1;
}

/* PORT PERF (2026-10-01): the parchment (noise per pixel) and the resampled map texture only change
 * with the room, but were repainted on every redraw - every other frame while Link moves (hardware
 * profile: the panel took 2-7% of Old 3DS time). They are cached here, keyed by what they depend on;
 * a redraw copies them and paints only the markers. */
static u16 sMapBase[320 * 240];
static u32 sMapBaseKey;
static int sMapBaseValid;
static float sMapBaseS, sMapBaseOx, sMapBaseOy;

static u32 MapKey(int have) {
    const PortMinimap* m = &gPortMinimap;
    u32 key = (have ? (u32)m->sceneId + 1 : 0) + (u32)sThemeRed * 0x40000000u; /* (the frame is the theme's) */
    if (!have) return key;
    for (int i = 0; i < m->numLayers; i++) {
        const PortMinimapLayer* layer = &m->layers[i];
        key = key * 33u + (u32)(uintptr_t)layer->tex;
        key = key * 33u + (u16)layer->x + ((u32)(u16)layer->y << 16);
        key = key * 33u + layer->w + ((u32)layer->h << 16);
        key = key * 33u + layer->fmt + layer->flipX * 8u + layer->flipY * 16u + layer->byteXor * 32u;
        key = key * 33u + layer->r + ((u32)layer->g << 8) + ((u32)layer->b << 16) + ((u32)layer->a << 24);
        const u8* tex = layer->tex;
        int n = (layer->w * layer->h + 1) / 2;
        for (int k = 0; k < 64; k++) {
            key = key * 33u + tex[(k * n / 64) ^ layer->byteXor];
        }
    }
    return key;
}

static void MapCopyColumns(u16* dst, const u16* src) {
    int x;
    for (x = MAP_X; x < MAP_X + MAP_W; x++) {
        int base = x * 240 + (239 - (MAP_Y + MAP_H - 1));
        memcpy(&dst[base], &src[base], MAP_H * sizeof(u16));
    }
}

static void DrawMapBase(int have, float* ps, float* pox, float* poy);

static void DrawMapInto(int have) {
    const PortMinimap* m = &gPortMinimap;
    float s, ox, oy;
    int x;
    u32 key = MapKey(have);
    if (!sMapBaseValid || key != sMapBaseKey) {
        DrawMapBase(have, &sMapBaseS, &sMapBaseOx, &sMapBaseOy);
        MapCopyColumns(sMapBase, sFb);
        sMapBaseKey = key;
        sMapBaseValid = 1;
    } else {
        MapCopyColumns(sFb, sMapBase);
    }
    sFbDirty = 1;
    if (!have) return;
    s = sMapBaseS, ox = sMapBaseOx, oy = sMapBaseOy;
#define MX(v) (ox + (v) * s)
#define MY(v) (oy + (v) * s)
    for (x = 0; x < m->numMarks; x++) {
        int mx = (int)MX(m->markX[x]), my = (int)MY(m->markY[x]);
        if (m->markType[x] == 0 /* MAP_MARK_CHEST */) {
            FillRect(mx, my, 9, 7, PRGB(70, 36, 16));
            FillRect(mx + 1, my + 1, 7, 5, PRGB(210, 60, 40));
            FillRect(mx + 1, my + 3, 7, 1, PRGB(240, 200, 60));
        } else {
            int size = m->markType[x] == 2 ? 4 : 3;
            FillRect(mx - size / 2, my - size / 2, size, size,
                     PRGB(m->markR[x], m->markG[x], m->markB[x]));
        }
    }
    if (m->hasStart) {
        MapTri(MX(m->startX), MY(m->startY), m->startYaw, 7, PRGB(200, 0, 0));
    }
    if (m->compass) {
        MapTri(MX(m->playerX), MY(m->playerY), m->playerYaw, 9, PRGB(40, 40, 20));
        MapTri(MX(m->playerX), MY(m->playerY), m->playerYaw, 7, PRGB(250, 240, 0));
    }
#undef MX
#undef MY
}

/* the parchment and the map texture, fitted to the frame (its scale/offset returned for the markers) */
static void DrawMapBase(int have, float* ps, float* pox, float* poy) {
    const PortMinimap* m = &gPortMinimap;
    float s, ox, oy, inv;
    int x, y, bx0, by0, bx1, by1, lum;
    *ps = 1.0f, *pox = 0.0f, *poy = 0.0f;
    /* the window under it darkened like the grid's cells, a light hairline (the design's look) */
    BackLayer(MAP_X, MAP_Y, MAP_W, MAP_H);
    FillRectA(MAP_X + 1, MAP_Y + 1, MAP_W - 2, MAP_H - 2, CellBgRGB(), 160);
    FrameA(MAP_X, MAP_Y, MAP_W, MAP_H, CellLineRGB(), 140);
    sFbDirty = 1;
    if (!have) return;
    /* fit the drawn part of the texture (plus the markers) to the frame: N64 maps sit in a corner of
     * their texture; shown large and centred */
    bx0 = by0 = 0x7FFF;
    bx1 = by1 = -0x7FFF;
    for (int i = 0; i < m->numLayers; i++) {
        const PortMinimapLayer* layer = &m->layers[i];
        for (y = 0; y < layer->h; y++) {
            for (x = 0; x < layer->w; x++) {
                if (MapTexel(layer, x, y, &lum) != 0) {
                    int px = x + layer->x, py = y + layer->y;
                    if (px < bx0) bx0 = px;
                    if (px > bx1) bx1 = px;
                    if (py < by0) by0 = py;
                    if (py > by1) by1 = py;
                }
            }
        }
    }
    if (bx1 < bx0 || by1 < by0) {
        bx0 = (int)m->playerX - 32;
        by0 = (int)m->playerY - 32;
        bx1 = bx0 + 63;
        by1 = by0 + 63;
    }
    bx1++, by1++;
    s = fminf((MAP_W - 16) / (float)(bx1 - bx0), (MAP_H - 16) / (float)(by1 - by0));
    if (s > 3.0f) s = 3.0f;
    ox = MAP_X + (MAP_W - (bx1 - bx0) * s) * 0.5f - bx0 * s;
    oy = MAP_Y + (MAP_H - (by1 - by0) * s) * 0.5f - by0 * s;
    inv = 1.0f / s;
    for (int i = 0; i < m->numLayers; i++) {
        const PortMinimapLayer* layer = &m->layers[i];
        int r = 150 + layer->r * 105 / 255, g = 160 + layer->g * 95 / 255, b = 190 + layer->b * 65 / 255; /* light on dark */
        for (y = MAP_Y + 2; y < MAP_Y + MAP_H - 2; y++) {
            int ty = (int)floorf((y + 0.5f - oy) * inv) - layer->y;
            if (ty < 0 || ty >= layer->h) continue;
            for (x = MAP_X + 2; x < MAP_X + MAP_W - 2; x++) {
                int tx = (int)floorf((x + 0.5f - ox) * inv) - layer->x, a;
                if (tx < 0 || tx >= layer->w) continue;
                a = MapTexel(layer, tx, ty, &lum);
                if (a == 0) continue;
                Px(x, y, Blend565(PxGet(x, y), r * lum / 255, g * lum / 255, b * lum / 255, a));
            }
        }
    }
    *ps = s, *pox = ox, *poy = oy;
}

/* ---- settings page (PORT 2026-10-26) ----
 * The SETUP pad opens it in the centre column (where the status and minimap are): one row per option, tap a row to
 * change it. Changes apply at once and are saved to settings.txt. */
enum { S_FPS, S_WIDE, S_EDGES, S_TOPHUD, S_AA, S_CSTICK, S_GYRO, S_AUDIO, S_DIM, S_DETAIL, S_COUNT };
/* PORT (2026-10-26): a second page, GAMEPLAY: the quality-of-life options from 2Ship2Harkinian (port_minimap.h).
 * PORT (2026-11-08): more of them (gPortEnh, G_ENH + ENH_*): three pages of ten on the cheats' tighter rows, the
 * eleventh row turns the page. */
/* PORT (2026-11-08): the debug rows (Texture dump, Ground debug) are gone from the release; they followed the options */
enum { G_FAST, G_SKIP, G_PUTAWAY, G_ARROW, G_OWL, G_DTIME, G_PSAVE, G_GUARD, G_ENH, G_COUNT = G_ENH + ENH_COUNT };
#define SET_X 44
#define SET_W 232
#define SET_TAB_Y 3
#define SET_TAB_H 17
#define SET_Y0 24
#define SET_ROW_H 16
#define SET_PITCH 18 /* 10 rows: y 24..203, above the tabs (y 208) */
static int sSetPage; /* 0 SYSTEM, 1 GAMEPLAY, 2 CHEATS */
/* PORT (2026-10-28): a third page, CHEATS (port_minimap.h), all off by default. Twenty entries in two halves of ten
 * on tighter rows, the eleventh row turns the half. The GIVE rows are one-shot. */
#define CHEAT_GIVES 10
#define CHEAT_ENTRIES (CHEAT_TOGGLES + CHEAT_GIVES)
#define CHEAT_PER_PAGE 10
#define CHEAT_NAV CHEAT_PER_PAGE /* the row that turns the half */
#define CHEAT_PITCH 16
#define CHEAT_ROW_H 14
static int sCheatHalf;
static int sGameHalf; /* the GAMEPLAY page shown, 0..2 */
#define PAGED_NAV CHEAT_NAV
static const char* const sCheatLabels[CHEAT_ENTRIES] = {
    "Infinite health", "Infinite magic", "Infinite rupees", "Infinite ammo", "Infinite small keys",
    "Moon jump (L+R)", "Stop time", "Unbreakable Razor", "Climb anywhere", "Hookshot anywhere",
    "Elegy anywhere", "Unrestricted items", "Infinite carrots", "Endless flower glide", "Fierce Deity anywhere",
    "No clip", "Infinite Deku hops", "Goron roll, no magic", "Always win minigames", "Visible grottos",
    "Give all items", "Give all masks", "Give all songs", "Max upgrades", "Remains + notebook",
    "20 hearts + defense", "All owl statues", "Reveal world map", "Bottles of fairies", "Dawn of the First Day",
};
static const char* const sSetLabels[S_COUNT] = {
    "Frame rate", "Widescreen", "HUD at edges", "Top HUD", "Anti-alias", "C-Stick", "Gyro aim", "Audio buffer",
    "Cutscene light", "Detail",
};
static const char* const sGameLabels[G_COUNT] = {
    "Fast text", "Skip cutscenes", "Instant put-away", "Arrow cycle (R)", "Keep owl saves", "Double Time hour",
    "Save on pause", "Guard sight",
    /* G_ENH + ENH_* (port_minimap.h order) */
    "Fast chests", "Faster songs", "Fast rupee count", "Faster transitions", "Fast transformation",
    "No Blast Mask wait", "Faster climbing", "Faster block pushing", "Underwater ocarina", "Side roll + crouch stab",
    "Owl warp from pause", "Sun's Song", "Takkuri can't steal", "No final-day quakes", "Wide actor culling",
    "Actor draw distance", "D-pad C buttons", "HUD size", "CRT filter", "Invert aim up/down", "Invert aim left/right",
    "Bottom screen",
};
static int sSetShownWide = -1; /* the Widescreen row as drawn (SELECT also toggles it) */
/* the Gyro aim row's buttons (x; same row height) */
#define GYRO_BTN_W 22
#define GYRO_PLUS_X (SET_X + SET_W - 4 - GYRO_BTN_W)
#define GYRO_MINUS_X (GYRO_PLUS_X - 44 - GYRO_BTN_W)
static int sGyroLastOn = 250; /* the strength gyro aiming comes back at after OFF */

/* the GAMEPLAY and CHEATS tabs are paged: ten entries a page, the eleventh row turns it */
static int PagedEntries(void) {
    return sSetPage == 2 ? CHEAT_ENTRIES : G_COUNT;
}
static int* PagedHalf(void) {
    return sSetPage == 2 ? &sCheatHalf : &sGameHalf;
}
static int PagedPages(void) {
    return (PagedEntries() + CHEAT_PER_PAGE - 1) / CHEAT_PER_PAGE;
}
static int CheatRowCount(void) { /* the entries on the shown page */
    int n = PagedEntries() - *PagedHalf() * CHEAT_PER_PAGE;
    return n > CHEAT_PER_PAGE ? CHEAT_PER_PAGE : n;
}

static int SetRowCount(void) {
    return sSetPage ? PAGED_NAV + 1 : S_COUNT;
}

static int SetRowY(int row) {
    return SET_Y0 + row * (sSetPage ? CHEAT_PITCH : SET_PITCH);
}

static int SetRowH(void) {
    return sSetPage ? CHEAT_ROW_H : SET_ROW_H;
}

static void CheatValue(int e, char* buf, size_t n) {
    if (e >= CHEAT_TOGGLES) {
        snprintf(buf, n, "GIVE");
    } else if (e == CHEAT_TIMESTOP) {
        static const char* const sStop[4] = { "OFF", "TEMPLES", "DUNGEONS", "ALWAYS" };
        snprintf(buf, n, "%s", sStop[gPortCheats[e] & 3]);
    } else {
        snprintf(buf, n, "%s", gPortCheats[e] ? "ON" : "OFF");
    }
}

static void SettingValue(int row, char* buf, size_t n) {
    extern int gPortAA, gPortAudioMargin;
    if (sSetPage && row == PAGED_NAV) {
        snprintf(buf, n, "%d/%d  >", *PagedHalf() + 1, PagedPages());
        return;
    }
    if (sSetPage == 2) {
        CheatValue(sCheatHalf * CHEAT_PER_PAGE + row, buf, n);
        return;
    }
    if (sSetPage) {
        extern int gPortFastText, gPortSkipCutscenes, gPortInstantPutaway, gPortArrowCycle, gPortOwlPersist,
            gPortDoubleTime;
        extern int gPortPauseSave, gPortGuardSight;
        row = sGameHalf * CHEAT_PER_PAGE + row; /* (the entry) */
        if (row == G_ENH + ENH_ACTOR_DISTANCE) {
            if (gPortEnh[ENH_ACTOR_DISTANCE]) snprintf(buf, n, "%dX", 1 + gPortEnh[ENH_ACTOR_DISTANCE]);
            else snprintf(buf, n, "OFF");
            return;
        }
        if (row == G_ENH + ENH_HUD_SIZE) {
            static const int kPct[7] = { 100, 110, 120, 130, 70, 80, 90 };
            int s = gPortEnh[ENH_HUD_SIZE];
            snprintf(buf, n, "%d%%", kPct[s >= 0 && s <= 6 ? s : 0]);
            return;
        }
        if (row == G_ENH + ENH_CRT) {
            snprintf(buf, n, "%s", gPortEnh[ENH_CRT] == 1 ? "LIGHT" : gPortEnh[ENH_CRT] == 2 ? "STRONG" : "OFF");
            return;
        }
        if (row == G_ENH + ENH_BOTTOM_OFF) {
            snprintf(buf, n, "%s", gPortEnh[ENH_BOTTOM_OFF] ? "OFF" : "ON");
            return;
        }
        if (row >= G_ENH && row < G_ENH + ENH_COUNT) {
            snprintf(buf, n, "%s", gPortEnh[row - G_ENH] ? "ON" : "OFF");
            return;
        }
        if (row == G_GUARD) {
            snprintf(buf, n, "%s", gPortGuardSight == 1 ? "NEVER" : gPortGuardSight == 2 ? "ALWAYS" : "NIGHT");
            return;
        }
        int v = row == G_FAST ? gPortFastText : row == G_SKIP ? gPortSkipCutscenes : row == G_PUTAWAY ? gPortInstantPutaway
              : row == G_ARROW ? gPortArrowCycle : row == G_OWL ? gPortOwlPersist : row == G_DTIME ? gPortDoubleTime
              : gPortPauseSave;
        snprintf(buf, n, "%s", v ? "ON" : "OFF");
        return;
    }
    switch (row) {
        case S_FPS:    snprintf(buf, n, "%s", sInterp ? "60 FPS" : "20 FPS"); break;
        case S_WIDE:   snprintf(buf, n, "%s", gPortWidescreen ? "ON" : "OFF"); break;
        case S_EDGES:  snprintf(buf, n, "%s", gPortHudEdges ? "ON" : "OFF"); break;
        case S_TOPHUD: snprintf(buf, n, "%s", gPortHudTop ? "ON" : "OFF"); break;
        case S_AA:     snprintf(buf, n, "%s", gPortAA ? "ON" : "OFF"); break;
        case S_CSTICK: snprintf(buf, n, "%s", sCstickCamera ? "CAMERA" : "C-BTNS"); break;
        case S_GYRO:
            if (sGyroSensitivityPercent <= 0) snprintf(buf, n, "OFF");
            else snprintf(buf, n, "%d%%", sGyroSensitivityPercent);
            break;
        case S_AUDIO:
            snprintf(buf, n, "%s", gPortAudioMargin < 0 ? "AUTO" : gPortAudioMargin == 0 ? "SMALL" :
                                   gPortAudioMargin <= 256 ? "MEDIUM" : gPortAudioMargin <= 512 ? "LARGE" : "MAX");
            break;
        case S_DIM:
            if (kDimPct[sCutsceneDim] == 0) snprintf(buf, n, "OFF");
            else snprintf(buf, n, "%d%%", kDimPct[sCutsceneDim]);
            break;
        case S_DETAIL: snprintf(buf, n, "%s", gPortHiDetail ? "MAX" : "NORMAL"); break;
        default:       buf[0] = '\0'; break;
    }
}

/* a line of text left-aligned at x, its capitals centred in a box h pixels tall at y */
static void PTextLMid(int x, int y, int h, const char* s, int f, u32 rgb, int shadow) {
    PTextB(x, y + (h + kPFonts[f].asc * 3 / 4 + 1) / 2, s, f, rgb, shadow, 0);
}

static void DrawSettingRow(int row) {
    int y = SetRowY(row), h = SetRowH(), give = 0;
    char val[16];
    int on;
    const char* label;
    if (sSetPage) {
        int e = *PagedHalf() * CHEAT_PER_PAGE + row;
        if (row == PAGED_NAV) {
            label = sSetPage == 2 ? "More cheats" : "More options";
            give = 1;
        } else if (row >= CheatRowCount()) { /* past the last entry of the last page: an empty strip */
            FillRect(SET_X, y, SET_W, h, ColBg());
            sFbDirty = 1;
            return;
        } else if (sSetPage == 2) {
            label = sCheatLabels[e];
            give = e >= CHEAT_TOGGLES;
        } else {
            label = sGameLabels[e];
        }
    } else {
        label = sSetLabels[row];
    }
    /* a row: the file select's button stretched across (the GIVE rows and the turn bronze), the value in gold (OFF
     * dimmed) */
    FillRect(SET_X, y, SET_W, h, ColBg());
    ButtonFace(SET_X, y, SET_W, h, give ? RGB24(210, 150, 80) : ThemeTint());
    PTextLMid(SET_X + 7, y, h, label, F_9B, COL_WHITE, 1);
    SettingValue(row, val, sizeof(val));
    on = strcmp(val, "OFF") != 0;
    if (!sSetPage && (row == S_GYRO || row == S_DIM)) {
        /* [-] value [+]: the buttons step it; tapping the label or value turns gyro aiming off / back on (the
         * cutscene light: no dimming / back) */
        int bx;
        for (bx = 0; bx < 2; bx++) {
            int px = bx == 0 ? GYRO_MINUS_X : GYRO_PLUS_X;
            ButtonFace(px, y + 2, GYRO_BTN_W, SET_ROW_H - 4, RGB24(255, 211, 77));
            PTextMid(px + GYRO_BTN_W / 2, y + 2, SET_ROW_H - 4, bx == 0 ? "-" : "+", F_9B, COL_INK, 0);
        }
        PTextMid((GYRO_MINUS_X + GYRO_BTN_W + GYRO_PLUS_X) / 2, y, h, val, F_9B, on ? COL_GOLD24 : COL_OFF, 1);
    } else {
        PTextB(SET_X + SET_W - 7 - PTextW(F_9B, val, 0), y + (h + kPFonts[F_9B].asc * 3 / 4 + 1) / 2, val, F_9B,
               on ? COL_GOLD24 : COL_OFF, 1, 0);
    }
    if (!sSetPage && row == S_WIDE) sSetShownWide = gPortWidescreen != 0;
    sFbDirty = 1;
}

static void DrawSettingsTabs(void) {
    int w = (SET_W - 8) / 3, t;
    static const char* const sTabs[3] = { "System", "Gameplay", "Cheats" };
    FillRect(SET_X - 2, 0, SET_W + 4, SET_Y0 - 1, ColBg());
    for (t = 0; t < 3; t++) {
        int x = SET_X + t * (w + 4);
        if (t == sSetPage) ArtDraw9(&sArt[A_HILITE], x - 2, SET_TAB_Y - 3, w + 4, SET_TAB_H + 6, 8, 8, 6, 6, RGB24(200, 240, 255), 230, 0);
        ButtonFace(x, SET_TAB_Y, w, SET_TAB_H, t == sSetPage ? ThemeTintHi() : ThemeTint());
        PTextMid(x + w / 2, SET_TAB_Y, SET_TAB_H, sTabs[t], F_9B, t == sSetPage ? COL_WHITE : COL_HINT, 1);
    }
}

static void DrawSettingsPage(void) {
    int i;
    FillRect(WIN_X, 0, 240, 240, ColBg()); /* the settings page: the whole centre, top to bottom */
    DrawSettingsTabs();
    for (i = 0; i < SetRowCount(); i++) DrawSettingRow(i);
    if (sSetPage == 1) {
        PTextC(SET_X + SET_W / 2, SET_Y0 + (PAGED_NAV + 1) * CHEAT_PITCH + 6, "From 2Ship2Harkinian", F_8S, COL_OFF, 1);
    }
}

static int SettingsRowAt(int px, int py) {
    int i;
    if (!sSettingsOpen || px < SET_X || px >= SET_X + SET_W) return -1;
    if (py >= SET_TAB_Y && py < SET_TAB_Y + SET_TAB_H) { /* the page tabs */
        int t = (px - SET_X) * 3 / SET_W;
        return 100 + (t > 2 ? 2 : t);
    }
    for (i = 0; i < SetRowCount(); i++) {
        int y = SetRowY(i);
        if (py >= y && py < y + SetRowH()) return i;
    }
    return -1;
}

static void SettingsTap(int row, int px) {
    extern int gPortAA, gPortAudioMargin;
    if (row >= 100) { /* a page tab */
        if (sSetPage != row - 100) {
            sSetPage = row - 100;
            sTouchUiRedraw = 1;
        }
        return;
    }
    if (sSetPage && row == PAGED_NAV) { /* turn the page */
        *PagedHalf() = (*PagedHalf() + 1) % PagedPages();
        sTouchUiRedraw = 1;
        return;
    }
    if (sSetPage && row >= CheatRowCount()) return;
    if (sSetPage == 2) {
        int e = sCheatHalf * CHEAT_PER_PAGE + row;
        if (e >= CHEAT_TOGGLES) { /* GIVE: done by the game thread (ultra_shims.c PortCheat_Update) */
            gPortCheatGive = CHEAT_GIVE_ITEMS + (e - CHEAT_TOGGLES);
            return;
        }
        gPortCheats[e] = e == CHEAT_TIMESTOP ? (gPortCheats[e] + 1) & 3 : !gPortCheats[e];
        Port3ds_SaveSettings();
        if (!sDimmed) DrawSettingRow(row);
        return;
    }
    if (sSetPage) {
        extern int gPortFastText, gPortSkipCutscenes, gPortInstantPutaway, gPortArrowCycle, gPortOwlPersist,
            gPortDoubleTime;
        extern int gPortPauseSave, gPortGuardSight;
        int shownRow = row;
        row = sGameHalf * CHEAT_PER_PAGE + row; /* (the entry) */
        if (row == G_ENH + ENH_ACTOR_DISTANCE || row == G_ENH + ENH_HUD_SIZE || row == G_ENH + ENH_CRT) {
            /* the stepped rows: actor distance OFF..5X, HUD size 100 110 120 130 70 80 90 %, CRT OFF LIGHT STRONG */
            int e = row - G_ENH, steps = e == ENH_ACTOR_DISTANCE ? 5 : e == ENH_HUD_SIZE ? 7 : 3;
            gPortEnh[e] = (gPortEnh[e] + 1) % steps;
            Port3ds_SaveSettings();
            if (!sDimmed) DrawSettingRow(shownRow);
            return;
        }
        if (row >= G_ENH && row < G_ENH + ENH_COUNT) {
            gPortEnh[row - G_ENH] = !gPortEnh[row - G_ENH];
            Port3ds_SaveSettings();
            if (!sDimmed) DrawSettingRow(shownRow);
            return;
        }
        if (row == G_GUARD) { /* NIGHT (original) -> NEVER -> ALWAYS */
            gPortGuardSight = (gPortGuardSight + 1) % 3;
            Port3ds_SaveSettings();
            if (!sDimmed) DrawSettingRow(shownRow);
            return;
        }
        int* v = row == G_FAST ? &gPortFastText : row == G_SKIP ? &gPortSkipCutscenes : row == G_PUTAWAY ? &gPortInstantPutaway
               : row == G_ARROW ? &gPortArrowCycle : row == G_OWL ? &gPortOwlPersist : row == G_DTIME ? &gPortDoubleTime
               : &gPortPauseSave;
        *v = !*v;
        Port3ds_SaveSettings();
        if (!sDimmed) DrawSettingRow(shownRow);
        return;
    }
    switch (row) {
        case S_FPS:    sInterp = !sInterp; break;
        case S_WIDE:   gPortWidescreen = !gPortWidescreen; break;
        case S_EDGES:  gPortHudEdges = !gPortHudEdges; break;
        case S_TOPHUD: gPortHudTop = !gPortHudTop; break;
        case S_AA:     gPortAA = !gPortAA; break;
        case S_CSTICK: sCstickCamera = !sCstickCamera; break;
        case S_GYRO: /* [-] / [+] step by 25 (25..1000, below 25 is OFF); elsewhere on the row: OFF <-> on */
            if (px >= GYRO_PLUS_X - 2) {
                sGyroSensitivityPercent = sGyroSensitivityPercent <= 0 ? 25 : sGyroSensitivityPercent + 25;
                if (sGyroSensitivityPercent > 1000) sGyroSensitivityPercent = 1000;
            } else if (px >= GYRO_MINUS_X - 2 && px < GYRO_MINUS_X + GYRO_BTN_W + 2) {
                sGyroSensitivityPercent = sGyroSensitivityPercent <= 25 ? 0 : sGyroSensitivityPercent - 25;
            } else if (sGyroSensitivityPercent > 0) {
                sGyroLastOn = sGyroSensitivityPercent;
                sGyroSensitivityPercent = 0;
            } else {
                sGyroSensitivityPercent = sGyroLastOn > 0 ? sGyroLastOn : 250;
            }
            if (sGyroSensitivityPercent > 0) sGyroLastOn = sGyroSensitivityPercent;
            break;
        case S_AUDIO: /* AUTO -> SMALL -> MEDIUM -> LARGE -> MAX (samples kept queued: audio_3ds.c) */
            gPortAudioMargin = gPortAudioMargin < 0 ? 0 : gPortAudioMargin == 0 ? 256 : gPortAudioMargin <= 256 ? 512 :
                               gPortAudioMargin <= 512 ? 1024 : -1;
            break;
        case S_DIM: { /* [-] / [+]: a step darker / lighter; elsewhere on the row: no dimming <-> the last level */
            static int sDimLast = 3;
            if (px >= GYRO_PLUS_X - 2) {
                if (sCutsceneDim < DIM_LEVELS - 1) sCutsceneDim++;
            } else if (px >= GYRO_MINUS_X - 2 && px < GYRO_MINUS_X + GYRO_BTN_W + 2) {
                if (sCutsceneDim > 0) sCutsceneDim--;
            } else if (sCutsceneDim < DIM_LEVELS - 1) {
                sDimLast = sCutsceneDim;
                sCutsceneDim = DIM_LEVELS - 1;
            } else {
                sCutsceneDim = sDimLast;
            }
            if (sCutsceneDim < DIM_LEVELS - 1) sDimLast = sCutsceneDim;
            break;
        }
        case S_DETAIL: gPortHiDetail = !gPortHiDetail; break; /* NORMAL <-> MAX (the game's distance checks) */
        default:       return;
    }
    Port3ds_SaveSettings();
    if (!sDimmed) DrawSettingRow(row); /* (a dimmed panel is redrawn whole instead) */
}

/* ---- the pause panel (PORT 2026-10-26): while the game is paused the centre window offers SAVE ----
 * An owl save of where Link is, written at once, and the game goes on (ultra_shims.c PortEnh_PauseSave). It is loaded from
 * the file screen like any owl save. */
extern volatile int gPortPauseSaveReq, gPortPauseSaveResult, gPortCanPauseSave;
#define SAVE_BTN_X 100
#define SAVE_BTN_Y 134
#define SAVE_BTN_W 120
#define SAVE_BTN_H 28
static int sPauseShownCan = -1, sPauseShownResult = 0, sPauseSaveHeld;
static void DrawPausePanel(void) {
    int can = gPortCanPauseSave, res = gPortPauseSaveResult, cx = SAVE_BTN_X + SAVE_BTN_W / 2;
    BackLayer(CT_X, CT_Y, CT_W, CT_H);
    PTextC(cx, 110, "Paused", F_12B, COL_WHITE, 1);
    if (can && sPauseSaveHeld) {
        ArtDraw9(&sArt[A_HILITE], SAVE_BTN_X - 4, SAVE_BTN_Y - 4, SAVE_BTN_W + 8, SAVE_BTN_H + 8, 8, 8, 6, 6,
                 RGB24(200, 240, 255), 230, 0);
    }
    ButtonFace(SAVE_BTN_X, SAVE_BTN_Y, SAVE_BTN_W, SAVE_BTN_H,
               !can ? RGB24(110, 110, 120) : sPauseSaveHeld ? ThemeTintHi() : ThemeTint());
    PTextMid(cx, SAVE_BTN_Y, SAVE_BTN_H, "Save", F_12B, can ? COL_WHITE : COL_OFF, 1);
    if (res > 0) {
        PTextC(cx, SAVE_BTN_Y + SAVE_BTN_H + 7, "Saved!", F_10S, RGB24(140, 255, 140), 1);
    } else if (res < 0 || !can) {
        PTextC(cx, SAVE_BTN_Y + SAVE_BTN_H + 7, "You can't save here.", F_9B, RGB24(255, 140, 110), 1);
    }
    PTextC(cx, 205, "Continue it from the owl save", F_8S, COL_HINT, 1);
    PTextC(cx, 215, "on the file select screen.", F_8S, COL_HINT, 1);
    sPauseShownCan = can, sPauseShownResult = res;
    sFbDirty = 1;
}

/* ---- the centre window's pages (PORT 2026-11-06) ---- */
static int sDragSlot = -1; /* the inventory slot (0..47) being dragged, -1 none */
static int sDragX, sDragY;
static const char* const kCTag[3] = { "Y", "I", "X" }; /* C-left, C-down, C-right */

/* a cell: the window darkened, a light hairline (gold while on a button) */
static void DrawCellFrame(int x, int y, int w, int h, int marked, int strong) {
    BackLayer(x, y, w, h);
    FillRectA(x + 1, y + 1, w - 2, h - 2, CellBgRGB(), 140);
    FrameA(x, y, w, h, marked ? COL_GOLD24 : CellLineRGB(), marked ? 255 : strong ? 230 : 140);
}

static void DrawGridCell(int slot) {
    int base = sPage == PG_MASKS ? 24 : 0, k = slot - base, c;
    int x = GRID_X + (k % 6) * (GRID_CW + GRID_GX), y = GRID_Y + (k / 6) * (GRID_CH + GRID_GY);
    int item = sShown.valid ? sShown.inv[slot] : 0xFF, marked = 0;
    for (c = 0; c < 3; c++) marked |= item != 0xFF && sShown.cSlot[c] == slot;
    if (base && item != 0xFF && item == sShown.mask) marked = 1;
    DrawCellFrame(x, y, GRID_CW, GRID_CH, marked, slot == sDragSlot);
    sFbDirty = 1;
    if (item == 0xFF) return;
    DrawIcon(x + 2, y + 1, Port_GetItemIconCached(item), 24, (!base && sShown.invDim[k]) || slot == sDragSlot);
    if (!base && sShown.invAmmo[k] >= 0) DrawAmmo(x + GRID_CW - 1, y + GRID_CH - 9, sShown.invAmmo[k], sShown.invAmmoFull[k], 1);
    for (c = 0; c < 3; c++) {
        if (sShown.cSlot[c] == slot) PText(x + 2, y + 1, kCTag[c], F_6B, COL_GOLD24, 1, 0);
    }
    if (base && item == sShown.mask) PText(x + 2, y + 1, "II", F_6B, COL_GOLD24, 1, 0);
}

static void DrawPageContent(void) {
    int k;
    if (sSettingsOpen) return;
    if (sPausePanel) {
        DrawPausePanel();
        return;
    }
    BackLayer(CT_X, CT_Y, CT_W, CT_H);
    switch (sPage) {
        case PG_ITEMS:
        case PG_MASKS:
            for (k = 0; k < 24; k++) DrawGridCell((sPage == PG_MASKS ? 24 : 0) + k);
            /* (no "Drag an item onto ..." hint under the grid: user request, 2026-11-08) */
            break;
        case PG_MAP:
            DrawMap(sMapLastHave);
            break;
        case PG_QUEST: {
            char buf[16];
            int have = sShown.valid && sShown.notebook, sw;
            PText(GRID_X, 107, "Remains", F_9B, COL_WHITE, 1, 0);
            for (k = 0; k < 4; k++) { /* the file select's remains, faint until owned */
                int x = GRID_X + k * 52, y = 120;
                DrawCellFrame(x, y, 42, 36, 0, 0);
                ArtDraw(&sArt[A_REMAINS0 + k], x + 5, y + 2, COL_WHITE, sShown.valid && (sShown.remains & (1 << k)) ? 256 : 56);
            }
            PText(GRID_X, 163, "Songs", F_9B, COL_WHITE, 1, 0);
            snprintf(buf, sizeof(buf), "%d / 10", sShown.valid ? sShown.songs : 0);
            PTextR(GRID_X + 198, 163, buf, F_9B, COL_WHITE, 1);
            sw = PTextW(F_9B, "Songs", 0);
            FillRectA(GRID_X + sw + 6, 169, 198 - sw - 12 - PTextW(F_9B, buf, 0), 1, CellLineRGB(), 140);
            /* the Bomber's Notebook (it opens as when used from a C button) */
            ButtonFace(NOTE_X, NOTE_Y, NOTE_W, NOTE_H, have ? ThemeTint() : RGB24(110, 110, 120));
            DrawIcon(NOTE_X + 5, NOTE_Y + 3, gPortHudSerial ? Port_GetItemIcon(0x61) : NULL, 18, !have);
            PTextLMid(NOTE_X + 27, NOTE_Y, NOTE_H, "Bombers' Notebook", F_9B, have ? COL_WHITE : COL_OFF, 1);
            break;
        }
    }
    sFbDirty = 1;
}

/* the tabs: the file select's blank buttons on the window, the shown page's with the highlight behind it */
static void DrawTabs(void) {
    int i;
    for (i = P_TAB_ITEMS; i <= P_TAB_QUEST; i++) {
        const PanelPad* p = &sPads[i];
        if (p->page == sPage) ArtDraw(&sArt[A_HILITE], p->x - 2, TAB_Y - 4, RGB24(200, 240, 255), 230);
        ArtDraw(&sArt[A_BLANK], p->x, TAB_Y, ThemeTint(), 256);
        PTextMid(p->x + 26, TAB_Y, 16, p->label, F_9B, COL_WHITE, 1);
    }
    if (sTouchUiN3ds) { /* ZR: the mask slot's button, a yellow badge on Masks */
        const PanelPad* p = &sPads[P_TAB_MASKS];
        int w = PTextW(F_6B, "ZR", 0) + 6, bx = p->x + 55 - w, by = TAB_Y - 5;
        FillRect(bx, by, w, 10, PRGB(0x10, 0x18, 0x3a));
        FillRect(bx + 1, by + 1, w - 2, 8, PRGB(0xff, 0xe9, 0x4d));
        PTextMid(bx + w / 2, by + 1, 8, "ZR", F_6B, COL_INK, 0);
    }
    sFbDirty = 1;
}

/* the centre: the window, its tabs and the page (or the settings page over the whole centre) */
static void DrawCenter(void) {
    if (sSettingsOpen) {
        DrawSettingsPage();
        return;
    }
    BackLayer(WIN_X, WIN_Y, 240, 240 - WIN_Y);
    DrawTabs();
    DrawPageContent();
}

/* the slot of the Items / Masks grid cell under (px, py), -1 none */
static int GridSlotAt(int px, int py) {
    int col, row;
    if (sSettingsOpen || sPausePanel || (sPage != PG_ITEMS && sPage != PG_MASKS)) return -1;
    if (px < GRID_X || py < GRID_Y) return -1;
    col = (px - GRID_X) / (GRID_CW + GRID_GX);
    row = (py - GRID_Y) / (GRID_CH + GRID_GY);
    if (col > 5 || row > 3) return -1;
    if ((px - GRID_X) % (GRID_CW + GRID_GX) >= GRID_CW || (py - GRID_Y) % (GRID_CH + GRID_GY) >= GRID_CH) return -1;
    return (sPage == PG_MASKS ? 24 : 0) + row * 6 + col;
}

/* the dragged icon follows the finger: drawn over the panel last, the pixels under it kept to put back first */
#define DRAG_SZ 30
static u16 sDragPatch[DRAG_SZ * DRAG_SZ];
static int sDragDrawn, sDragPX, sDragPY;
static void DragErase(void) {
    int i, j;
    if (!sDragDrawn) return;
    for (j = 0; j < DRAG_SZ; j++)
        for (i = 0; i < DRAG_SZ; i++) Px(sDragPX + i, sDragPY + j, sDragPatch[j * DRAG_SZ + i]);
    sDragDrawn = 0;
    sFbDirty = 1;
}
static void DragDraw(int px, int py, int item) {
    int i, j;
    sDragPX = px - DRAG_SZ / 2, sDragPY = py - DRAG_SZ / 2;
    for (j = 0; j < DRAG_SZ; j++)
        for (i = 0; i < DRAG_SZ; i++) sDragPatch[j * DRAG_SZ + i] = PxGet(sDragPX + i, sDragPY + j);
    DrawIcon(sDragPX + 1, sDragPY + 1, Port_GetItemIconCached(item), 28, 0);
    sDragDrawn = 1;
    sFbDirty = 1;
}

/* ---- dimming in cutscenes (PORT 2026-10-26) ----
 * During cutscenes the panel stays up, dimmed, with what gameplay last showed (hearts, map), and keeps working. A
 * cutscene must last a few updates first, so a door or a chest does not flash the panel. */
static int PanelDimWanted(const PortHudInfo* now) {
    static int sCsPolls;
    int cs = (sCutsceneDim < DIM_LEVELS - 1) && !gPortInPause && (now->cutscene || sHudStale >= 4); /* (pause: not one) */
    if (!cs) {
        sCsPolls = 0;
        return 0;
    }
    if (sCsPolls < 1000) sCsPolls++;
    return sCsPolls >= 8; /* ~0.4 s at 20 updates per second */
}

static void DimPanel(void) { /* the panel at kDimPct[sCutsceneDim] of its brightness (0: black) */
    int i, k = kDimPct[sCutsceneDim] * 256 / 100;
    if (k <= 0) {
        memset(sFb, 0, 320 * 240 * 2);
    } else if (k == 128) {
        for (i = 0; i < 320 * 240; i++) sFb[i] = (sFb[i] >> 1) & 0x7BEF; /* RGB565 at half brightness */
    } else {
        for (i = 0; i < 320 * 240; i++) {
            u32 c = sFb[i];
            sFb[i] = (u16)(((((c >> 11) * k) >> 8) << 11) | (((((c >> 5) & 63) * k) >> 8) << 5) | (((c & 31) * k) >> 8));
        }
    }
    sFbDirty = 1;
}

/* PORT (2026-11-08): the bottom screen's backlight, off in cutscenes at the lowest cutscene light. It is turned back on
 * before the HOME menu, sleep and quitting (Port3ds_CpuStateHook) and set again by the next poll. */
static int sBottomLightOff;
static void Port3ds_BottomLight(int on) {
    static int sLcd = -1;
    if (on == !sBottomLightOff) return;
    if (sLcd < 0) sLcd = R_SUCCEEDED(gspLcdInit());
    if (!sLcd) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    sBottomLightOff = !on;
}

static void Port3ds_TouchUiInit(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    { extern void PortCompat_SilenceStderr(void); PortCompat_SilenceStderr(); }
    sTouchUi = 1;
    sTouchUiN3ds = n3ds;
    sTouchUiRedraw = 1; /* drawn on the first poll: renderer init (in the game loop) clears the screen */
}

/* PORT (2026-10-04): taps between updates. Input is read once per update; at low update rates (the Old 3DS, a heavy
 * scene) a quick tap on a touch pad could start and end between two reads and be lost (hardware v52 at ~11 updates
 * per second: the SCREEN pad "did nothing" on the title screen). The game thread also reads the buttons at each audio
 * pump (once per retrace, Port3ds_ScanBetweenUpdates); a touch or ZR press seen there is kept, with where the touch
 * landed, until the next update's touch panel poll. The game's own controller still reads the buttons once per
 * update, as on the N64. */
static u32 sTapDownAcc;      /* KEY_TOUCH / KEY_ZR presses since the last update's poll */
static touchPosition sTapPos; /* where that touch landed */
static int sGyroReady = -1;
static float sGyroCoefficient;
static u64 sGyroTick;
static float sGyroPitchAccum, sGyroYawAccum;
static float sGyroPitchFrame, sGyroYawFrame;
static u32 sGyroFrame, sGyroReadFrame;
static int sGyroWasActive;

static void Port3ds_GyroExit(void) {
    HIDUSER_DisableGyroscope();
}

static void Port3ds_SampleGyro(void) {
    if (sGyroReady != 1) {
        return;
    }
    u64 now = svcGetSystemTick();
    float elapsed = (float)(now - sGyroTick) / 268111856.0f;
    sGyroTick = now;
    if (elapsed > 0.1f) {
        sGyroPitchAccum = sGyroYawAccum = 0.0f;
        sGyroWasActive = 0;
        return;
    }
    angularRate rate;
    hidGyroRead(&rate);
    float pitch = rate.x / sGyroCoefficient;
    float yaw = rate.y / sGyroCoefficient;
    float scale = elapsed * (65536.0f / 360.0f) * (sGyroSensitivityPercent / 100.0f);
    if (fabsf(pitch) >= 0.5f) {
        sGyroPitchAccum += pitch * scale;
    }
    if (fabsf(yaw) >= 0.5f) {
        sGyroYawAccum += yaw * scale;
    }
}

int Port3ds_ReadGyro(int active, float* pitch, float* yaw) {
    *pitch = *yaw = 0.0f;
    if (sGyroSensitivityPercent <= 0) {
        active = 0; /* settings: gyro aiming OFF (the stick aims as on the N64) */
    }
    if (active && sGyroReady < 0) {
        sGyroReady = 0;
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&sGyroCoefficient)) &&
            sGyroCoefficient > 0.0f && R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
            sGyroReady = 1;
            sGyroTick = svcGetSystemTick();
            atexit(Port3ds_GyroExit);
        }
    }
    if (!active || sGyroReady != 1) {
        sGyroWasActive = 0;
        return 0;
    }
    if (sGyroWasActive && sGyroFrame - sGyroReadFrame == 1) {
        *pitch = sGyroPitchFrame;
        *yaw = sGyroYawFrame;
    }
    sGyroReadFrame = sGyroFrame;
    sGyroWasActive = 1;
    return 1;
}

void Port3ds_ScanBetweenUpdates(void) {
    u32 d;
    if (Port3ds_OnRenderThread()) {
        return;
    }
    hidScanInput();
    Port3ds_SampleGyro();
    d = hidKeysDown() & (KEY_TOUCH | KEY_ZR);
    if ((d & KEY_TOUCH) && !(sTapDownAcc & KEY_TOUCH)) {
        hidTouchRead(&sTapPos);
    }
    sTapDownAcc |= d;
}

/* a new ZR mask is kept for the next boot (each save file's, ultra_shims.c PortMaskSlot_Update) */
static void SaveMaskSlotIfNew(void) {
    static unsigned sSavedMaskSerial;
    if (gPortMaskSlotSerial != sSavedMaskSerial) {
        sSavedMaskSerial = gPortMaskSlotSerial;
        Port3ds_SaveSettings();
    }
}

static int sWakeTouch; /* the touch that turned the bottom screen back on is still down (ENH_BOTTOM_OFF) */

/* held pads -> N64 bits; tap actions; redraws what changed */
static unsigned short Port3ds_TouchUiPoll(void) {
    static unsigned sPolls;
    int hit = -1, i, iconsOk;
    PortHudInfo now;
    if (!sTouchUi) return 0;
    sPolls++;
    /* PORT (2026-11-07): the panel is drawn off screen and copied to the screen whole when done. The bottom screen is
     * single-buffered (consoleInit) and scanned out while we draw: a page drawn in place showed its in-between states
     * for a few frames - the dark background before the window art over it - and the console's adaptive backlight
     * (power-saving mode) answered that dark moment by brightening the whole screen for half a second: the "flash" at
     * every tap (user video, 2026-11-07). */
    sFb = sPanelFb;
    if (gPortTouchOcarina > 0) gPortTouchOcarina--; /* requests expire if the player could not act */

    { /* PORT (2026-11-14, asked for): the bottom screen off (Gameplay > Bottom screen OFF): black, its backlight off,
       * nothing drawn and no pad pressed. A tap anywhere on it turns it back on; that touch presses nothing until the
       * finger lifts (sWakeTouch). ZR still assigns a mask on the pause screen. */
        static int sBlanked;
        if (gPortEnh[ENH_BOTTOM_OFF]) {
            u32 down = hidKeysDown() | sTapDownAcc;
            if (!sBlanked) {
                u16* screen = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
                memset(screen, 0, 320 * 240 * 2);
                Port3ds_CacheFlush(screen, 320 * 240 * 2);
                sBlanked = 1;
            }
            Port3ds_BottomLight(0);
            if (gPortInPause && (down & KEY_ZR)) gPortMaskAssignReq = 1;
            sTapDownAcc = 0;
            sHeldPad = -1;
            SaveMaskSlotIfNew();
            if (down & KEY_TOUCH) {
                gPortEnh[ENH_BOTTOM_OFF] = 0;
                Port3ds_SaveSettings();
                sWakeTouch = 1;
                sTouchUiRedraw = 1; /* drawn whole at the next poll; the dim block sets the backlight again */
            }
            return 0;
        }
        sBlanked = 0;
    }

    /* freshness of the game-side data */
    sHudStale = (gPortHudSerial != sLastHudSerial) ? 0 : sHudStale + 1;
    sLastHudSerial = gPortHudSerial;
    sMapStale = (gPortMinimap.serial != sLastMapSerial) ? 0 : sMapStale + 1;
    sLastMapSerial = gPortMinimap.serial;

    Port_GetHudInfo(&now);
    if (sHudStale >= 4) {
        now.valid = 0;
        now.rupees = now.keys = -1;
        now.ocarina = now.mask = 0xFF;
    }

    { /* cutscenes: the panel dims (and is redrawn whole when that changes) */
        int dim = PanelDimWanted(&now);
        if (dim != sDimmed) {
            sDimmed = dim;
            sTouchUiRedraw = 1;
        }
        Port3ds_BottomLight(!(sDimmed && kDimPct[sCutsceneDim] == 0)); /* the lowest level: the screen off */
    }

    { /* paused: the pause panel (SAVE) takes the map's place */
        extern int gPortPauseSave;
        int want = gPortPauseSave && gPortInPause && !sSettingsOpen && gPortHudSerial != 0;
        if (want != sPausePanel) {
            sPausePanel = want;
            if (want) gPortPauseSaveResult = 0;
            sTouchUiRedraw = 1;
        }
    }
    ArtReady(); /* the game's art, read from the ROM at the first poll */
    DragErase(); /* the dragged icon is drawn again last (DragDraw) */
    { /* the final day turns the panel red: everything again */
        int red = (sDimmed ? sShown.valid && sShown.day == 3 : now.valid && now.day == 3);
        if (red != sThemeRed) sTouchUiRedraw = 1;
    }

    if (sTouchUiRedraw) {
        sTouchUiRedraw = 0;
        if (!sDimmed) sShown = now; /* dimmed: keep showing what gameplay last showed */
        sThemeRed = sShown.valid && sShown.day == 3;
        FillRect(0, 0, 320, 240, ColBg());
        sShownIconsOk = IconsOk();
        sShownIconSeg = gPortHudIconSeg;
        for (i = 0; i < P_TAB_ITEMS; i++) DrawPad(i);
        DrawHeader();
        DrawCenter();
        sMapForce = 1; /* the minimap is drawn again from the game's data at the next chance */
        if (sDimmed) DimPanel();
    }

    int saveTouch = 0;
    {
        u32 down = hidKeysDown() | sTapDownAcc;
        int held = (hidKeysHeld() & KEY_TOUCH) != 0, setRow = -1;
        if (sWakeTouch) { /* the tap that turned the screen back on: ignored until the finger lifts */
            if (!held && !(down & KEY_TOUCH)) sWakeTouch = 0;
            held = 0;
            down &= ~KEY_TOUCH;
        }
        if (held || (down & KEY_TOUCH)) {
            touchPosition tp;
            if (sTapDownAcc & KEY_TOUCH) {
                tp = sTapPos; /* the tap that started since the last poll (maybe already over) */
            } else {
                hidTouchRead(&tp);
            }
            for (i = 0; i < P_COUNT; i++) {
                const PanelPad* p = &sPads[i];
                if (p->page >= 0 && sSettingsOpen) continue; /* the tabs are under the settings page */
                if (tp.px >= p->x && tp.px < p->x + p->w && tp.py >= p->y && tp.py < p->y + p->h) hit = i;
            }
            if (down & KEY_TOUCH) {
                int slot = GridSlotAt(tp.px, tp.py);
                /* an item or mask picked up from its page (in gameplay, not in a cutscene or the pause menu) */
                if (slot >= 0 && sShown.inv[slot] != 0xFF && now.valid && !gPortInPause && !sDimmed && sDragSlot < 0) {
                    sDragSlot = slot;
                    DrawGridCell(slot);
                }
            }
            if (sDragSlot >= 0) {
                /* dragging: over X / Y / I (or II for a mask) it is dropped there; no button is pressed meanwhile */
                int over = -1;
                if (hit >= P_CLEFT && hit <= P_MASKS && (hit != P_MASKS || sDragSlot >= 24)) over = hit;
                hit = -1;
                if (over != sDragHover) {
                    int old = sDragHover;
                    sDragHover = over;
                    if (old >= 0) DrawPad(old);
                    if (over >= 0) DrawPad(over);
                }
                sDragX = tp.px, sDragY = tp.py;
            }
            if (hit < 0 && sDragSlot < 0 && (down & KEY_TOUCH)) {
                setRow = SettingsRowAt(tp.px, tp.py);
                if (setRow >= 0) {
                    SettingsTap(setRow, tp.px);
                    if (sDimmed) sTouchUiRedraw = 1; /* a dimmed panel is redrawn whole (next poll) */
                }
                if (!sSettingsOpen && !sPausePanel && sPage == PG_QUEST && tp.px >= NOTE_X && tp.px < NOTE_X + NOTE_W &&
                    tp.py >= NOTE_Y && tp.py < NOTE_Y + NOTE_H) {
                    extern volatile int gPortNotebookReq;
                    gPortNotebookReq = 1; /* ultra_shims.c opens the Bomber's Notebook (or buzzes) */
                }
            }
            if (hit < 0 && sPausePanel && tp.px >= SAVE_BTN_X && tp.px < SAVE_BTN_X + SAVE_BTN_W &&
                tp.py >= SAVE_BTN_Y && tp.py < SAVE_BTN_Y + SAVE_BTN_H) {
                saveTouch = 1;
                if ((down & KEY_TOUCH) && gPortCanPauseSave) gPortPauseSaveReq = 1;
            }
        }
        if (!held && sDragSlot >= 0) { /* the finger lifted: drop */
            int slot = sDragSlot, target = sDragHover;
            sDragSlot = -1;
            sDragHover = -1;
            if (target == P_MASKS) {
                gPortMaskSlotReq = sShown.inv[slot] + 1; /* the II (ZR) mask slot */
            } else if (target >= P_CLEFT && target <= P_CRIGHT) {
                gPortEquipReq = ((target - P_CLEFT + 1) << 8) | (slot + 1); /* EQUIP_SLOT_C_LEFT .. C_RIGHT */
            }
            if (target >= 0) DrawPad(target);
            if (!sDimmed) DrawGridCell(slot);
        }
    }
    if ((hidKeysDown() | sTapDownAcc) & KEY_TOUCH) {
        if (hit == P_SETUP) {
            sSettingsOpen = !sSettingsOpen;
            sTouchUiRedraw = 1; /* the centre column changes: redrawn whole at the next poll */
        } else if (hit == P_OCARINA && now.valid && now.ocarina == PANEL_ITEM_OCARINA_OF_TIME) {
            gPortTouchOcarina = 3;
        } else if (hit == P_MASKS && (gPortMaskSlotItem == 0xFF || !now.valid) && !sSettingsOpen) {
            if (sPage != PG_MASKS) { /* nothing on II yet: the Masks page, to drag one onto it */
                sPage = PG_MASKS;
                if (!sDimmed) DrawCenter();
                else sTouchUiRedraw = 1;
            }
        } else if (hit >= 0 && sPads[hit].page >= 0 && sPads[hit].page != sPage) {
            sPage = sPads[hit].page;
            if (!sDimmed) DrawCenter();
            else sTouchUiRedraw = 1;
            sMapForce = 1;
        }
    }
    if ((hidKeysDown() | sTapDownAcc) & KEY_ZR) {
        /* PORT (2026-10-26): ZR is the mask slot (port_minimap.h). On the pause screen it puts the mask under the cursor
         * on ZR; in gameplay it wears that mask (the controller bit, below), or opens the Masks page while ZR is empty */
        if (gPortInPause) {
            gPortMaskAssignReq = 1; /* (an empty slot also fills itself with the first mask worn: ultra_shims.c) */
        }
    }
    sTapDownAcc = 0;
    SaveMaskSlotIfNew();
    if (sDimmed) {
        /* dimmed: nothing is drawn piece by piece (it would show bright); the pads still work */
        sHeldPad = hit;
        goto panel_flush;
    }
    if (hit != sHeldPad) {
        int old = sHeldPad;
        sHeldPad = hit;
        if (old >= 0 && sPads[old].page < 0) DrawPad(old);
        if (hit >= 0 && sPads[hit].page < 0) DrawPad(hit);
    }

    iconsOk = IconsOk();
    /* C pads: item/ammo/disabled changes, and a few refreshes a second while icons may still be
     * arriving by async DMA after an equip */
    if (memcmp(now.cItem, sShown.cItem, 3) || memcmp(now.cDisabled, sShown.cDisabled, 3) ||
        memcmp(now.cAmmo, sShown.cAmmo, sizeof(now.cAmmo)) || memcmp(now.cAmmoFull, sShown.cAmmoFull, 3) ||
        iconsOk != sShownIconsOk ||
        (iconsOk && (sShownIconSeg != gPortHudIconSeg || (sPolls % 20) == 0))) {
        memcpy(sShown.cItem, now.cItem, 3);
        memcpy(sShown.cDisabled, now.cDisabled, 3);
        memcpy(sShown.cAmmo, now.cAmmo, sizeof(now.cAmmo));
        memcpy(sShown.cAmmoFull, now.cAmmoFull, 3);
        sShownIconsOk = iconsOk;
        sShownIconSeg = gPortHudIconSeg;
        for (i = 0; i < 3; i++) DrawPad(P_CLEFT + i);
    }
    int validityChanged = now.valid != sShown.valid;
    sShown.valid = now.valid;
    if (now.mask != sShown.mask || validityChanged) sShown.mask = now.mask, DrawPad(P_MASKS);
    if (sPausePanel && (gPortCanPauseSave != sPauseShownCan || gPortPauseSaveResult != sPauseShownResult ||
                        saveTouch != sPauseSaveHeld)) {
        sPauseSaveHeld = saveTouch;
        DrawPausePanel();
    }
    {   /* the OCARINA button: owned, and the final hours' glow */
        static int sShownFinal = -1;
        if (now.ocarina != sShown.ocarina || validityChanged || FinalHours() != sShownFinal) {
            sShown.ocarina = now.ocarina;
            sShownFinal = FinalHours();
            DrawPad(P_OCARINA);
        }
    }
    if (sSettingsOpen && sSetPage == 0 && (gPortWidescreen != 0) != sSetShownWide) DrawSettingRow(S_WIDE); /* SELECT toggled it */
    {
        int tatl = gPortHudTatl && sHudStale < 4;
        static int sTatlPulseT;
        if (tatl != sShownTatl) {
            sShownTatl = tatl;
            sTatlPulse = 1, sTatlPulseT = 0;
            DrawPad(P_VIEW);
        } else if (tatl && ++sTatlPulseT >= 8) {
            sTatlPulseT = 0;
            sTatlPulse ^= 1;
            DrawPad(P_VIEW);
        }
    }
    if (validityChanged || now.health != sShown.health || now.healthCapacity != sShown.healthCapacity ||
        now.magic != sShown.magic || now.magicCapacity != sShown.magicCapacity || now.day != sShown.day ||
        now.minutes != sShown.minutes || now.rupees != sShown.rupees || now.keys != sShown.keys ||
        now.wallet != sShown.wallet || now.rupeesFull != sShown.rupeesFull || now.defHearts != sShown.defHearts ||
        strcmp(now.name, sShown.name) != 0) {
        sShown.health = now.health, sShown.healthCapacity = now.healthCapacity;
        sShown.magic = now.magic, sShown.magicCapacity = now.magicCapacity;
        sShown.day = now.day, sShown.minutes = now.minutes;
        sShown.rupees = now.rupees, sShown.keys = now.keys;
        sShown.wallet = now.wallet, sShown.rupeesFull = now.rupeesFull, sShown.defHearts = now.defHearts;
        memcpy(sShown.name, now.name, sizeof(sShown.name));
        sShown.lifeSize = now.lifeSize;
        memcpy(sShown.beatPrim, now.beatPrim, 3), memcpy(sShown.beatEnv, now.beatEnv, 3);
        DrawHeader();
    } else if (now.valid && !sSettingsOpen && BeatingHeart() >= 0 &&
               (now.lifeSize != sShown.lifeSize || memcmp(now.beatPrim, sShown.beatPrim, 3) ||
                memcmp(now.beatEnv, sShown.beatEnv, 3))) { /* the beating heart */
        sShown.lifeSize = now.lifeSize;
        memcpy(sShown.beatPrim, now.beatPrim, 3), memcpy(sShown.beatEnv, now.beatEnv, 3);
        RedrawHearts();
    }
    {   /* the pages' contents: the items and masks, what is on the buttons, remains, songs, the notebook, the day */
        static u32 sShownPageSig;
        u32 sig = 2166136261u;
        const u8* b;
        size_t k;
#define SIG_BYTES(ptr, n) for (b = (const u8*)(ptr), k = 0; k < (size_t)(n); k++) sig = (sig ^ b[k]) * 16777619u
        SIG_BYTES(now.inv, sizeof(now.inv));
        SIG_BYTES(now.invAmmo, sizeof(now.invAmmo));
        SIG_BYTES(now.invAmmoFull, sizeof(now.invAmmoFull));
        SIG_BYTES(now.invDim, sizeof(now.invDim));
        SIG_BYTES(now.cSlot, sizeof(now.cSlot));
        sig = (sig ^ (u32)now.mask) * 16777619u;
        sig = (sig ^ ((u32)now.remains | (u32)now.songs << 8 | (u32)now.notebook << 16 | (u32)now.valid << 17)) * 16777619u;
        sig = (sig ^ (u32)(now.day * 4 + (now.day == 3 && now.minutes < 6 * 60))) * 16777619u;
#undef SIG_BYTES
        if (sig != sShownPageSig) {
            sShownPageSig = sig;
            memcpy(sShown.inv, now.inv, sizeof(now.inv));
            memcpy(sShown.invAmmo, now.invAmmo, sizeof(now.invAmmo));
            memcpy(sShown.invAmmoFull, now.invAmmoFull, sizeof(now.invAmmoFull));
            memcpy(sShown.invDim, now.invDim, sizeof(now.invDim));
            memcpy(sShown.cSlot, now.cSlot, sizeof(now.cSlot));
            sShown.remains = now.remains, sShown.songs = now.songs, sShown.notebook = now.notebook;
            if (sPage != PG_MAP) DrawPageContent();
        }
    }

    { /* minimap: redrawn every other poll while the game feeds it, cleared once when it stops */
        static int sMapShown;
        static u32 sMapSig;
        if (sMapStale == 0 && (sPolls & 1)) {
            /* redraw only when something visible changed (arrows move by whole panel pixels) */
            const PortMinimap* m = &gPortMinimap;
            int have = m->numLayers || m->compass || m->hasStart || m->numMarks;
            u32 sig = MapKey(have) + m->compass * 17u + m->hasStart * 23u +
                      (u32)(int)(m->playerX * 2.0f) * 101u + (u32)(int)(m->playerY * 2.0f) * 1009u +
                      (u32)(m->playerYaw >> 11) * 10007u + (u32)(int)m->startX * 3u + (u32)(int)m->startY * 5u +
                      m->numMarks * 19u + (u32)(u16)m->startYaw * 31u;
            for (int k = 0; k < m->numMarks; k++) {
                sig = sig * 33u + (u16)m->markX[k] + ((u32)(u16)m->markY[k] << 16);
                sig = sig * 33u + m->markType[k] + ((u32)m->markR[k] << 8) +
                      ((u32)m->markG[k] << 16) + ((u32)m->markB[k] << 24);
            }
            if (!sMapShown || sig != sMapSig || sMapForce) {
                DrawMap(have);
                sMapSig = sig;
                sMapForce = 0;
            }
            sMapShown = 1;
        } else if (sMapStale > 6 && sMapShown) {
            DrawMap(0);
            sMapShown = 0;
        }
    }

panel_flush:
    if (sDragSlot >= 0 && !sDimmed) DragDraw(sDragX, sDragY, sShown.inv[sDragSlot]);
    {
        u16* screen = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (sFbDirty) {
            sFbDirty = sFbMapDirty = 0;
            memcpy(screen, sPanelFb, sizeof(sPanelFb));
            Port3ds_CacheFlush(screen, 240 * 320 * 2);
        } else {
            if (sDirtyX1 > sDirtyX0) { /* a part alone (the beating heart): its columns */
                memcpy(screen + sDirtyX0 * 240, sPanelFb + sDirtyX0 * 240, (sDirtyX1 - sDirtyX0) * 240 * 2);
                Port3ds_CacheFlush(screen + sDirtyX0 * 240, (sDirtyX1 - sDirtyX0) * 240 * 2);
            }
            if (sFbMapDirty) { /* the minimap's redraws while Link moves: its columns only */
                sFbMapDirty = 0;
                memcpy(screen + MAP_X * 240, sPanelFb + MAP_X * 240, MAP_W * 240 * 2);
                Port3ds_CacheFlush(screen + MAP_X * 240, MAP_W * 240 * 2);
            }
        }
        sDirtyX0 = 320, sDirtyX1 = 0;
    }

    { /* verification aid: with sdmc:/3ds/mm/capture_bottom present, dump the panel every 300 polls
       * (raw framebuffer + header, overwritten) so tools can check it without screenshots */
        if ((sPolls % 300) == 0) {
            FILE* flag = fopen("sdmc:/3ds/mm/capture_bottom", "rb");
            if (flag != NULL) {
                u16 w, h;
                u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
                FILE* out = fopen("sdmc:/3ds/mm/bottom_fb.bin", "wb");
                fclose(flag);
                if (out != NULL) {
                    u32 hdr[3] = { w, h, (u32)gfxGetScreenFormat(GFX_BOTTOM) };
                    fwrite(hdr, 4, 3, out);
                    fwrite(fb, 1, (size_t)w * h * gspGetBytesPerPixel(gfxGetScreenFormat(GFX_BOTTOM)), out);
                    fclose(out);
                }
            }
        }
    }
    /* the II button presses the mask bit only while a mask is on it (ZR's slot), and not on the pause screen */
    if (hit == P_MASKS && (gPortMaskSlotItem == 0xFF || gPortInPause)) return 0;
    return hit >= 0 ? sPads[hit].btn : 0;
}


s8 gPortCamX, gPortCamY;     /* C-stick camera input for this update (z_camera.c) */
int gPortMenuInput;          /* > 0 while a menu is up (set each update by the game: file select, pause) */
int gPortDpadDir;            /* the D-pad item pressed (port_minimap.h PORT_BTN_DPAD) */
int gPortDpadAsC;            /* > 0 while a message or the ocarina staff is up (set each update by the game, z_play.c) */
static void Port3ds_PollInput(void) {
    hidScanInput();
    Port3ds_SampleGyro();
    sGyroPitchFrame = sGyroPitchAccum;
    sGyroYawFrame = sGyroYawAccum;
    sGyroPitchAccum = sGyroYawAccum = 0.0f;
    sGyroFrame++;
    u32 k = hidKeysHeld();
    /* PORT (2026-09-30): the button layout, complete on the Old 3DS (no ZL/ZR, no C-stick):
     *   A/B = A/B, L = Z-target, R = shield, Y/X = C-left/C-right, D-pad = C-up/C-down/C-left/C-right
     *   (the N64 D-pad is unused by OoT), SELECT = N64 L (minimap), START = START, touch panel =
     *   VIEW/C buttons/OCARINA/BOOTS/pause tabs/SCREEN + the minimap (docs/3ds-touch-panel.md).
     * New 3DS extras: C-stick = camera (settings cstick=0: the four C buttons), ZL = C-down (item slot I),
     * ZR = BOOTS (cycles the owned boots, like tapping the pad; 2026-10-03, it was a third C-up). */
    unsigned short b = 0;
    if (k & KEY_A)      b |= BTN_A_;
    if (k & KEY_B)      b |= BTN_B_;
    if (k & KEY_Y)      b |= BTN_CLEFT_;
    if (k & KEY_X)      b |= BTN_CRIGHT_;
    if (k & KEY_START)  b |= BTN_START_;
    if (k & KEY_SELECT) b |= BTN_L_;
    if (k & KEY_L)      b |= BTN_Z_;
    if (k & KEY_R)      b |= BTN_R_;
    if (k & KEY_ZL)     b |= BTN_CDOWN_;
    int menu = gPortMenuInput > 0;
    if (gPortMenuInput > 0) gPortMenuInput--;
    int dpadC = menu || gPortDpadAsC > 0 || PORT_ENH(ENH_DPAD_CBTNS); /* (menu: the D-pad is the stick, below) */
    if (gPortDpadAsC > 0) gPortDpadAsC--;
    {
        /* the ZR mask slot. With the D-pad items, ZR + up is Fierce Deity's Mask, so the slot's mask goes on when ZR is
         * let go, unless the D-pad was pressed while ZR was held (a one-update press: input is polled once per update) */
        static int sZrHeld, sZrCombo;
        const int zr = (k & KEY_ZR) != 0;
        if (dpadC) {
            if (zr && !gPortInPause && gPortMaskSlotItem != 0xFF) b |= PORT_BTN_MASK;
            sZrCombo = 0;
        } else {
            if (zr && (k & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))) sZrCombo = 1;
            if (!zr && sZrHeld && !sZrCombo && !gPortInPause && gPortMaskSlotItem != 0xFF) b |= PORT_BTN_MASK;
            if (!zr) sZrCombo = 0;
        }
        sZrHeld = zr;
    }
    /* ZR: the BOOTS pad (Port3ds_TouchUiPoll); C-up stays on VIEW and D-pad up */
    /* PORT (2026-10-06, asked for): in menus (file select, the pause screens, the Bombers' Notebook:
     * gPortMenuInput, set by the game each update) the D-pad moves like the stick, as menus are navigated on the
     * N64; it presses no C button there. In gameplay it uses the D-pad items; in dialogue and ocarina playing it is the
     * C buttons. */
    {
        static int sDpadHeldOver; /* held since a menu, message or the ocarina staff: no item until it is let go */
        const int dpad = (k & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0;
        if (!menu && dpadC) {
            if (k & KEY_DUP)    b |= BTN_CUP_;
            if (k & KEY_DDOWN)  b |= BTN_CDOWN_;
            if (k & KEY_DLEFT)  b |= BTN_CLEFT_;
            if (k & KEY_DRIGHT) b |= BTN_CRIGHT_;
        }
        if (dpadC) {
            sDpadHeldOver = dpad;
        } else if (!dpad) {
            sDpadHeldOver = 0;
        } else if (!sDpadHeldOver) { /* the D-pad items (port_minimap.h PORT_BTN_DPAD) */
            gPortDpadDir = (k & KEY_DUP) ? ((k & KEY_ZR) ? 5 : 1) : (k & KEY_DDOWN) ? 2 : (k & KEY_DLEFT) ? 3 : 4;
            b |= PORT_BTN_DPAD;
        }
    }
    /* C-stick (New 3DS): turns the camera (z_camera.c Camera_Normal1) - PORT (2026-10-03), asked for on
     * hardware; settings cstick=0 makes it the four C buttons again (they are on Y/X/ZL/ZR/D-pad too) */
    gPortCamX = gPortCamY = 0;
    {
        static int sIrrst = -1;
        circlePosition cs;
        if (sIrrst < 0) sIrrst = R_SUCCEEDED(irrstInit());
        if (sIrrst) {
            irrstScanInput();
            irrstCstickRead(&cs);
        }
        if (sCstickCamera) {
            if (sIrrst) {
                /* range about +-146; a dead zone, then -127..127 */
                gPortCamX = (s8)(abs(cs.dx) < 20 ? 0 : (cs.dx > 146 ? 127 : cs.dx < -146 ? -127 : cs.dx * 127 / 146));
                gPortCamY = (s8)(abs(cs.dy) < 20 ? 0 : (cs.dy > 146 ? 127 : cs.dy < -146 ? -127 : cs.dy * 127 / 146));
            }
        } else if (sIrrst) {
            /* PORT (2026-11-14): the C buttons from the stick's position, as the camera reads it. The system's C-stick
             * key bits (KEY_CSTICK_*) never showed left on hardware (user report: up, down and right worked). Past
             * 40% of the range; the stronger axis only, so a slightly diagonal push is one C button. */
            const int ax = abs(cs.dx), ay = abs(cs.dy);
            if (ax > 58 || ay > 58) {
                if (ax >= ay) b |= cs.dx < 0 ? BTN_CLEFT_ : BTN_CRIGHT_;
                else b |= cs.dy < 0 ? BTN_CDOWN_ : BTN_CUP_;
            }
        } else {
            if (k & KEY_CSTICK_UP)    b |= BTN_CUP_;
            if (k & KEY_CSTICK_DOWN)  b |= BTN_CDOWN_;
            if (k & KEY_CSTICK_LEFT)  b |= BTN_CLEFT_;
            if (k & KEY_CSTICK_RIGHT) b |= BTN_CRIGHT_;
        }
    }
    b |= Port3ds_TouchUiPoll();

    circlePosition cp;
    hidCircleRead(&cp);
    /* circle pad range ~ +-156; scale to N64 +-80 */
    s3dsStickX = (signed char)(cp.dx * 80 / 156);
    s3dsStickY = (signed char)(cp.dy * 80 / 156);
    if (menu) { /* the D-pad as a full stick tilt (the circle pad still works) */
        if (k & KEY_DLEFT)  s3dsStickX = -80;
        if (k & KEY_DRIGHT) s3dsStickX = 80;
        if (k & KEY_DUP)    s3dsStickY = 80;
        if (k & KEY_DDOWN)  s3dsStickY = -80;
    }
    s3dsButtons = b;
}

/* render hooks — override sched_shim's weak PortGfx_RunTask (the 3DS analogue
 * of pc_gfx.c). Drives the citro3d backend via the gfx_pc interface. */
static int sGfxInited = 0;

void PortGfx_Init(void) {
    gfx_init(&gfx_3ds, &gfx_citro3d_api);
    sGfxInited = 1;
    { extern void PortDbg(const char*); PortDbg("[gfx] citro3d renderer initialized"); }
}

void PortGfx_FrameReady(void) {}

/* PORT (2026-09-24): N64-faithful pacing. OoT advances its logic once every R_UPDATE_RATE VI
 * retraces (3 -> 20 updates/s) and the N64 AudioMgr runs on EVERY retrace (60/s), independent of
 * the game. The port used to run one update per rendered frame (measured 26-29/s -> game ~1.4x too
 * fast, speed varying with scene load) and pumped audio once per update. Here: after presenting,
 * wait whole retraces until R_UPDATE_RATE retrace periods have passed since the previous update,
 * pumping audio once per retrace. A frame that renders late simply runs late (like N64 lag). */
static unsigned sPortAudioPumps = 0;
/* PORT PERF (2026-09-30): audio pumps also run DURING rendering. On the N64 the audio thread preempts the
 * game whenever a retrace arrives; here pumps only ran after the frame, so a slow frame (Old 3DS: 100 ms)
 * caught up with 5-6 back-to-back pumps, each waiting for the previous microcode task on the worker
 * core (measured 17-31 ms of main-thread waiting per frame, O3DS-sim). The display-list walker calls
 * Port3ds_MaybePumpAudio every 256 commands: one pump per elapsed retrace, so the worker's microcode
 * overlaps the rest of the frame. The display list is complete by then; audio only touches audio state. */
static double sLastPumpMs;  /* time of the last pump (any source) */
static int sMidFramePumps;  /* pumps since the previous Port3ds_PaceFrame */
#define PORT_RETRACE_MS (1000.0 / 59.83)
void Port3ds_MaybePumpAudio(void) {
    extern void Port3ds_PumpAudio(void);
    double now = (double)osGetTime();
    if (Port3ds_OnRenderThread()) {
        return; /* audio belongs to the game thread, which pumps it while it paces (render thread) */
    }
    if (sLastPumpMs != 0.0 && now - sLastPumpMs >= PORT_RETRACE_MS) {
        Port3ds_PumpAudio();
        sMidFramePumps++;
        sLastPumpMs += PORT_RETRACE_MS;
        if (now - sLastPumpMs > 4 * PORT_RETRACE_MS) sLastPumpMs = now; /* far behind: don't burst */
    }
}
/* PORT PERF (2026-10-02): pacing runs on svcGetSystemTick in milliseconds (osGetTime has whole-millisecond
 * steps: a frame finishing 50.4 ms after the update's start read as 50 < the 50.14 ms budget and waited one
 * more retrace - exposed by the present gate, whose last frame goes out just after the third vblank) */
static double Port3ds_NowMs(void) {
    return (double)svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000.0);
}
static double sLast = 0.0; /* when the current update started (retrace-aligned with the present gate) */
int gPortInterpOn; /* this update shows in-between frames (z_actor.c draws actors a margin past the edges) */
/* PORT (2026-10-01): frame skip (PortGfx_RunTask). sBehindMs = how far the game is behind the N64's
 * schedule of one update every R_UPDATE_RATE retraces; kept only while frame skip is on (sSkipOn). */
static double sBehindMs;
static int sSkipOn, sSkippedLast;
static unsigned sSkipCount, sLateUpdates;
static int Port3ds_UpdateRate(void) {
    extern void* gRegEditor;
    int rate = 3;
    if (gRegEditor) rate = *(short*)((char*)gRegEditor + 0x14 + 126 * 2); /* R_UPDATE_RATE = SREG(30) */
    if (rate < 1) rate = 1;
    if (rate > 6) rate = 6;
    return rate;
}
extern void Port3ds_VBlankSeen(void); /* gfx_3ds.c: vblank clock anchor for the present gate */
/* PORT PERF (2026-10-03): with the flip presenter (gfx_3ds.c) every frame carries the vblank it is due at: the
 * update's frames are spread over its R_UPDATE_RATE vblanks starting R_UPDATE_RATE vblanks after the update's
 * start (one update of latency: the first frame has a whole update period to be drawn) */
static void Port3ds_SetSlotAt(double start, int j, int frames, int rate) {
    extern double Port3ds_VBlankAtOrBefore(double tMs);
    extern void Port3ds_SetFrameSlot(double slotMs);
    const double kRetraceMs = 1000.0 / 59.831;
    double base = Port3ds_VBlankAtOrBefore(start + 1.0);
    Port3ds_SetFrameSlot(start == 0.0 ? 0.0 : base + (rate + (rate * j + frames / 2) / frames) * kRetraceMs);
}
/* wait whole retraces (pumping audio on each) until `targetMs` is under one retrace away */
static void Port3ds_WaitUntil(double targetMs) {
    extern void Port3ds_PumpAudio(void);
    while (Port3ds_NowMs() + 2.0 < targetMs) {
        gspWaitForVBlank();
        Port3ds_VBlankSeen();
        Port3ds_PumpAudio();
        sLastPumpMs = (double)osGetTime();
        sMidFramePumps++;
    }
}
static void Port3ds_PaceFrame(void) {
    extern void* gRegEditor;
    extern void Port3ds_PumpAudio(void);
    const double kRetraceMs = 1000.0 / 59.83; /* 3DS LCD refresh */
    int rate = 3;
    if (gRegEditor) rate = *(short*)((char*)gRegEditor + 0x14 + 126 * 2); /* R_UPDATE_RATE = SREG(30) */
    if (rate < 1) rate = 1;
    if (rate > 6) rate = 6;
    int pumped = sMidFramePumps; /* already done during rendering */
    double now;
    /* with frame skip, time owed from late frames is made up by not waiting for it */
    const double budget = rate * kRetraceMs - (sSkipOn ? sBehindMs : 0.0);
    sMidFramePumps = 0;
    for (;;) {
        /* PORT (2026-09-30): check the budget BEFORE waiting. The loop used to wait for a retrace
         * first, so a frame that had already used its budget (hardware: ~44 ms of work vs 50 ms)
         * still waited up to one more retrace: measured pace 14.7 ms/frame, 17.1 updates/s on a New
         * 3DS instead of 20. A late frame now goes straight on (audio catches up below). */
        if (sLast != 0.0 && Port3ds_NowMs() - sLast >= budget) break;
        gspWaitForVBlank();
        Port3ds_VBlankSeen();
        Port3ds_PumpAudio(); /* build+dispatch one audio RSP task per retrace, as on N64 */
        sLastPumpMs = (double)osGetTime();
        pumped++;
        if (sLast == 0.0 || Port3ds_NowMs() - sLast + 2.0 >= budget) break;
    }
    now = Port3ds_NowMs();
    if (sLast != 0.0 && now - sLast > rate * kRetraceMs + kRetraceMs) {
        sLateUpdates++; /* took more than one retrace longer than the N64's schedule (loads, heavy frames) */
    }
    if (sSkipOn && sLast != 0.0) {
        /* capped: a long stall (scene load) is not made up with a burst of skipped frames */
        sBehindMs += now - sLast - rate * kRetraceMs;
        if (sBehindMs < 0.0) sBehindMs = 0.0;
        if (sBehindMs > 2.0 * rate * kRetraceMs) sBehindMs = 2.0 * rate * kRetraceMs;
    } else {
        sBehindMs = 0.0;
    }
    /* Audio runs on wall-clock retraces, not on how many waits happened: if rendering ate most of
     * the budget, catch up to one audio task per retrace actually elapsed (capped). */
    if (sLast != 0.0) {
        int due = (int)((now - sLast) / kRetraceMs + 0.5);
        if (due > 8) due = 8;
        while (pumped < due) { Port3ds_PumpAudio(); pumped++; }
        sLastPumpMs = (double)osGetTime();
    }
    sPortAudioPumps += (unsigned)pumped;
    {
        /* With the present gate, an update that ends just after a vblank (its last frame is released
         * there) starts on that vblank: its frames are shown on vblanks, so the next update's budget
         * counts from one too. Not for a long overrun (Old 3DS, loads): that would count up to a retrace
         * of lateness that did not happen (frame skip's sBehindMs). +1 ms: the clock's anchor is taken
         * after gspWaitForVBlank returns, slightly after the vblank itself. */
        extern int gPortPresentGate;
        extern double Port3ds_VBlankAtOrBefore(double tMs);
        double vb = Port3ds_VBlankAtOrBefore(now + 1.0);
        sLast = gPortPresentGate && now - vb < 3.0 ? vb : now;
    }
}

/* PORT PERF (2026-09-28): per-frame CPU breakdown in 268 MHz system ticks, logged every 300 frames as
 * average microseconds per frame: game = everything between two graph tasks (game logic + DL build),
 * dl = display-list interpretation (gfx_run), swap = frame end/GPU wait, pace = retrace waits + audio;
 * plus triangles and draw calls sent to the GPU per frame (gfx_pc.c counters). */
u32 gPortPerfTris, gPortPerfDraws, gPortPerfTrisIn;
u64 gPortPerfAudioMain;
static u64 sPerfGame, sPerfDl, sPerfSwap, sPerfPace, sPerfLastEnd;
static u64 sPerfDlWait; /* the part of sPerfDl spent waiting: the walk's C3D_FrameBegin (gPortPerfGpuWait also
                         * counts the in-between frames' waits, which fall in "swap") and its present gate */
/* PORT PERF (2026-10-04): memory probe, once per perf report (~1 ms). The Old 3DS profile spends far more on writing
 * the vertex and GPU command buffers (linear memory) than its clock explains (hardware v49 "gpu pack": 12-17% of the
 * time at Old 3DS speed, 1% at New 3DS speed). Logged: ticks per 32-byte line (ARM11 ticks, 268 MHz) to write 64 KB
 * with word stores and with 32-byte block copies, to read it, and to flush it, for linear memory and for the heap.
 * Meaningful on hardware only (Azahar models no caches). */
static void Port3ds_MemProbe(void) {
    extern void PortDbgX(const char*, unsigned);
    enum { N = 64 * 1024, LINES = N / 32 };
    static u32* sLin;
    static u32 sHeapBuf[N / 4] __attribute__((aligned(32))); /* (the newlib heap has almost nothing free) */
    u32* sHeap = sHeapBuf;
    static const u32 src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u32* bufs[2];
    static const char* const names[2][4] = {
        { "perf mem probe linear: word stores ticks/line", "perf mem probe linear: block stores ticks/line",
          "perf mem probe linear: word loads ticks/line", "perf mem probe linear: flush ticks/line" },
        { "perf mem probe heap: word stores ticks/line", "perf mem probe heap: block stores ticks/line",
          "perf mem probe heap: word loads ticks/line", "perf mem probe heap: flush ticks/line" } };
    int b, i;
    if (sLin == NULL) sLin = (u32*)linearAlloc(N);
    if (sLin == NULL) return;
    bufs[0] = sLin, bufs[1] = sHeap;
    for (b = 0; b < 2; b++) {
        volatile u32* v = bufs[b];
        u64 t[5];
        u32 sum = 0;
        GSPGPU_FlushDataCache(bufs[1 - b], N); /* (touch the other buffer: this one's lines leave the L1) */
        for (i = 0; i < N / 4; i += 8) sum += ((volatile u32*)bufs[1 - b])[i];
        t[0] = svcGetSystemTick();
        for (i = 0; i < N / 4; i++) v[i] = (u32)i;
        t[1] = svcGetSystemTick();
        for (i = 0; i < N / 4; i += 8) memcpy(&bufs[b][i], src, 32);
        t[2] = svcGetSystemTick();
        for (i = 0; i < N / 4; i += 8) sum += v[i];
        t[3] = svcGetSystemTick();
        GSPGPU_FlushDataCache(bufs[b], N);
        t[4] = svcGetSystemTick();
        for (i = 0; i < 4; i++) PortDbgX(names[b][i], (unsigned)((t[i + 1] - t[i]) / LINES));
        if (sum == 0x12345678u) PortDbgX("perf mem probe (sum)", sum); /* (keeps the loads) */
    }
}

static void Port3ds_PerfReport(unsigned frames) {
    extern void PortDbgX(const char*, unsigned);
    const u64 div = (u64)frames * (SYSCLOCK_ARM11 / 1000000); /* ticks -> us per frame */
    PortDbgX("perf us/frame game", (unsigned)(sPerfGame / div));
    {
        extern u64 gPortPerfGpuWait;
        PortDbgX("perf us/frame dl (cpu)", (unsigned)((sPerfDl - sPerfDlWait) / div));
        PortDbgX("perf us/frame gpu wait", (unsigned)(gPortPerfGpuWait / div));
        gPortPerfGpuWait = 0;
    }
    {
        extern u64 gPortPerfTex, gPortPerfVtx, gPortPerfTri, gPortPerfFlush;
        extern u32 gPortPerfTexImports;
        PortDbgX("perf us/frame  dl.tex", (unsigned)(gPortPerfTex / div));
        PortDbgX("perf us/frame  dl.vtx", (unsigned)(gPortPerfVtx / div));
        PortDbgX("perf us/frame  dl.tri", (unsigned)(gPortPerfTri / div));
        PortDbgX("perf us/frame  dl.flush", (unsigned)(gPortPerfFlush / div));
        {
            extern u64 gPortPerfEmit, gPortPerfMtx;
            PortDbgX("perf us/frame  dl.tri.emit (clip+pack)", (unsigned)(gPortPerfEmit / div));
            PortDbgX("perf us/frame  dl.mtx", (unsigned)(gPortPerfMtx / div));
            gPortPerfEmit = gPortPerfMtx = 0;
        }
        PortDbgX("perf tex imports/frame", gPortPerfTexImports / frames);
        gPortPerfTex = gPortPerfVtx = gPortPerfTri = gPortPerfFlush = 0;
        gPortPerfTexImports = 0;
    }
    PortDbgX("perf us/frame swap", (unsigned)(sPerfSwap / div));
    PortDbgX("perf us/frame pace", (unsigned)(sPerfPace / div));
    {
        extern u64 gPortPerfAudioUcode, gPortPerfAudioWait, gPortPerfAudioMain;
        extern u32 gPortPerfAudioTasks;
        PortDbgX("perf us/frame  audio main (engine+wait)", (unsigned)(gPortPerfAudioMain / div));
        PortDbgX("perf us/frame  audio wait for ucode", (unsigned)(gPortPerfAudioWait / div));
        PortDbgX("perf us/task   audio ucode (worker)",
                 gPortPerfAudioTasks ? (unsigned)(gPortPerfAudioUcode / gPortPerfAudioTasks / (SYSCLOCK_ARM11 / 1000000)) : 0);
        gPortPerfAudioUcode = gPortPerfAudioWait = gPortPerfAudioMain = 0;
        { /* perf_stages: the 5 most expensive microcode ops, as op << 20 | us per frame */
            extern u64 gPortPerfAudOpTicks[32];
            extern int gPortPerfStagesOn;
            int k, i;
            for (k = 0; k < 5 && gPortPerfStagesOn; k++) {
                int best = 0;
                for (i = 1; i < 32; i++) if (gPortPerfAudOpTicks[i] > gPortPerfAudOpTicks[best]) best = i;
                if (gPortPerfAudOpTicks[best] == 0) break;
                PortDbgX("perf audio op<<20|us/frame", ((unsigned)best << 20) | (unsigned)(gPortPerfAudOpTicks[best] / div));
                gPortPerfAudOpTicks[best] = 0;
            }
            memset(gPortPerfAudOpTicks, 0, sizeof(gPortPerfAudOpTicks));
        }
        gPortPerfAudioTasks = 0;
    }
    PortDbgX("perf tris/frame", gPortPerfTris / frames);
    PortDbgX("perf tris in/frame (before clip+subdivision)", gPortPerfTrisIn / frames);
    { extern u32 gPortPerfSlowTris; PortDbgX("perf tris slow-path/frame", gPortPerfSlowTris / frames); gPortPerfSlowTris = 0; }
    gPortPerfTrisIn = 0;
    { /* memory budget (Old 3DS target: 96MB mode): linear free, regular heap in use (KB) */
        extern u32 linearSpaceFree(void);
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        struct mallinfo mi = mallinfo();
        static u32 sMinLinearFree = 0xFFFFFFFF;
        u32 lf = linearSpaceFree();
        if (lf < sMinLinearFree) sMinLinearFree = lf;
        PortDbgX("mem linear free KB", lf / 1024);
        PortDbgX("mem linear free min KB", sMinLinearFree / 1024);
        PortDbgX("mem heap used KB", (unsigned)mi.uordblks / 1024);
        PortDbgX("mem heap size KB", (unsigned)(fake_heap_end - fake_heap_start) / 1024);
    }
    PortDbgX("perf draws/frame", gPortPerfDraws / frames);
    { extern u32 gPortPerfRawTris; extern int gPortRawVtx; PortDbgX("perf raw vertex path", (unsigned)gPortRawVtx);
      PortDbgX("perf raw tris/frame", gPortPerfRawTris / frames); gPortPerfRawTris = 0;
      { extern u32 gPortPerfRawMaterialized; PortDbgX("perf raw vertices lit on the CPU (mixed tris)/frame", gPortPerfRawMaterialized / frames);
        gPortPerfRawMaterialized = 0; }
      { extern int C3Df_SwitchWords(int t); /* c3d_fast.c: words per switch to GPU << 16 | to raw; 0 = plain citro3d */
        PortDbgX("perf raw switch words gpu<<16|raw", ((unsigned)C3Df_SwitchWords(0) << 16) | (unsigned)C3Df_SwitchWords(1)); }
      { extern u32 gPortPerfRawSwitches; PortDbgX("perf raw program switches/frame", gPortPerfRawSwitches / frames);
        gPortPerfRawSwitches = 0; } }
    { extern u32 gPortPerfRoomTris; PortDbgX("perf room tris in/frame", gPortPerfRoomTris / frames); gPortPerfRoomTris = 0; }
#ifdef PORT_ACTOR_PROF
    { /* the 10 actor types whose drawing took longest (us per update), with their draw calls per update */
        extern u64 gPortActorTicks[512];
        extern u32 gPortActorCalls[512];
        int n, i;
        for (n = 0; n < 10; n++) {
            int best = -1;
            for (i = 0; i < 512; i++) if (gPortActorTicks[i] && (best < 0 || gPortActorTicks[i] > gPortActorTicks[best])) best = i;
            if (best < 0) break;
            PortDbgX("perf actor id<<16|us per update", ((unsigned)best << 16) |
                     (unsigned)(gPortActorTicks[best] / (SYSCLOCK_ARM11 / 1000000) / frames));
            PortDbgX("perf actor id<<16|calls x10 per update", ((unsigned)best << 16) | (gPortActorCalls[best] * 10 / frames));
            gPortActorTicks[best] = 0;
        }
        memset(gPortActorTicks, 0, sizeof(gPortActorTicks));
        memset(gPortActorCalls, 0, sizeof(gPortActorCalls));
    }
#endif
    { extern u64 gPortPerfRoomTicks; PortDbgX("perf us/frame in room geometry", (unsigned)(gPortPerfRoomTicks / (SYSCLOCK_ARM11 / 1000000) / frames));
      gPortPerfRoomTicks = 0; }
    { extern u32 gPortPerfAudioUnderruns, gPortPerfAudioDrops; extern int gPortAudioMargin, gPortAudioCoreNow;
      PortDbgX("perf audio underruns (DSP ran dry)", gPortPerfAudioUnderruns);
      PortDbgX("perf audio buffers dropped (queue full)", gPortPerfAudioDrops);
      { extern int gPortRawRelax; PortDbgX("perf speed rules active (raw relax, no shade split)", (unsigned)gPortRawRelax); }
      PortDbgX("perf audio margin samples", (unsigned)(gPortAudioMargin >= 0 ? gPortAudioMargin : gPortAudioCoreNow == 1 ? 512 : 0));
      gPortPerfAudioUnderruns = gPortPerfAudioDrops = 0; }
#ifdef PORT_PERF_STAGES
    { extern u32 gPortRawWhy[8]; static const char* const why[8] = { "perf raw-off vtx near/frame", "perf raw-off vtx deep/frame",
          "perf raw-off vtx lights/frame", "perf raw-off vtx skinned/frame", "perf raw-off vtx texgen/frame",
          "perf raw-off vtx identity/frame", "perf raw-off vtx range/frame", "perf raw vtx/frame" };
      int i; for (i = 0; i < 8; i++) { PortDbgX(why[i], gPortRawWhy[i] / frames); gPortRawWhy[i] = 0; } }
#endif
    { extern u32 gPortPerfReplayCopies, gPortPerfReplayCopyWords; extern int gPortReplayCopy;
      PortDbgX("perf replay copy on", (unsigned)gPortReplayCopy);
      PortDbgX("perf replay frames copied", gPortPerfReplayCopies);
      PortDbgX("perf replay copy words/frame", gPortPerfReplayCopies ? gPortPerfReplayCopyWords / gPortPerfReplayCopies : 0);
      gPortPerfReplayCopies = gPortPerfReplayCopyWords = 0;
      { extern u32 gPortPerfCopyChecked, gPortPerfCopyBad, gPortCopyBadInfo[6];
        if (gPortPerfCopyChecked) {
            PortDbgX("perf replay copy checked frames", gPortPerfCopyChecked);
            PortDbgX("perf replay copy MISMATCH frames", gPortPerfCopyBad);
            if (gPortPerfCopyBad) {
                PortDbgX("perf replay copy first mismatch at word", gPortCopyBadInfo[0]);
                PortDbgX("perf replay copy first mismatch log word", gPortCopyBadInfo[1]);
                PortDbgX("perf replay copy first mismatch copy word", gPortCopyBadInfo[2]);
                PortDbgX("perf replay copy lengths/4 log<<16|copy", gPortCopyBadInfo[3]);
                PortDbgX("perf replay copy mismatch register<<16|param", gPortCopyBadInfo[4]);
                PortDbgX("perf replay copy mismatch last uniform index", gPortCopyBadInfo[5]);

            }
            gPortPerfCopyChecked = gPortPerfCopyBad = 0;
        } } }
    { extern u32 gPortGpuRoute[4]; PortDbgX("perf gpu tris tested", gPortGpuRoute[0] / frames); PortDbgX("perf gpu tris behind eye", gPortGpuRoute[1] / frames); PortDbgX("perf gpu tris near", gPortGpuRoute[2] / frames); PortDbgX("perf gpu tris to cpu", gPortGpuRoute[3] / frames); gPortGpuRoute[0] = gPortGpuRoute[1] = gPortGpuRoute[2] = gPortGpuRoute[3] = 0; }
    { /* citro3d state calls per frame (each marks state citro3d re-sends with the next draw) */
        extern u32 gPortC3dCalls[6];
        static const char* const n[6] = { "perf c3d shader loads/frame", "perf c3d const sets/frame",
                                          "perf c3d tex binds same/frame", "perf c3d tex binds new/frame",
                                          "perf c3d sampler sets/frame", "perf c3d tex uploads/frame" };
        int q;
        for (q = 0; q < 6; q++) { PortDbgX(n[q], gPortC3dCalls[q] / frames); gPortC3dCalls[q] = 0; }
    }
    {
        extern u32 gPortPerfMemQueries, gPortPerfMemHits, gPortPerfDlCmds, gPortPerfDlCalls;
        PortDbgX("perf mem queries/frame (svc)", gPortPerfMemQueries / frames);
        PortDbgX("perf mem query cache hits/frame", gPortPerfMemHits / frames);
        PortDbgX("perf dl commands/frame", gPortPerfDlCmds / frames);
        PortDbgX("perf dl G_DL calls/frame", gPortPerfDlCalls / frames);
        gPortPerfMemQueries = gPortPerfMemHits = gPortPerfDlCmds = gPortPerfDlCalls = 0;
        { /* the 8 most frequent opcodes: opcode << 16 | count per frame */
            extern u32 gPortPerfOpCounts[256];
            int k, i;
            for (k = 0; k < 8; k++) {
                int best = -1;
                for (i = 0; i < 256; i++) if (best < 0 || gPortPerfOpCounts[i] > gPortPerfOpCounts[best]) best = i;
                if (gPortPerfOpCounts[best] == 0) break;
                PortDbgX("perf dl top opcode<<16|count", ((unsigned)best << 16) | (gPortPerfOpCounts[best] / frames));
                gPortPerfOpCounts[best] = 0;
            }
            memset(gPortPerfOpCounts, 0, sizeof(gPortPerfOpCounts));
        }
        { /* perf_stages: the 8 most expensive opcodes, opcode << 20 | us per frame */
            extern u64 gPortPerfOpTicks[256];
            extern int gPortPerfStagesOn;
            int k, i;
            for (k = 0; k < 8 && gPortPerfStagesOn; k++) {
                int best = 0;
                for (i = 1; i < 256; i++) if (gPortPerfOpTicks[i] > gPortPerfOpTicks[best]) best = i;
                if (gPortPerfOpTicks[best] == 0) break;
                PortDbgX("perf dl op<<20|us/frame", ((unsigned)best << 20) | (unsigned)(gPortPerfOpTicks[best] / div));
                gPortPerfOpTicks[best] = 0;
            }
            memset(gPortPerfOpTicks, 0, sizeof(gPortPerfOpTicks));
        }
    }
    if (sProfOn) { /* sampling profiler: per-mille of samples per stage */
        static const char* const names[PROF_COUNT] = {
            "prof game", "prof dl walk", "prof vtx", "prof tri setup", "prof tri build", "prof tri emit",
            "prof tex", "prof rect", "prof mtx", "prof flush", "prof audio", "prof pace", "prof swap",
            "prof gpu wait", "prof replay", "prof input", "prof vtx.light", "prof submit", "prof tri split",
            "prof gpu palette", "prof gpu pack", "prof vtx.box", "prof vtx.raw", "prof raw emit",
            "prof c3d context", "prof c3d uniforms", "prof c3d draw" };
        u32 tot = 0;
        int i;
        for (i = 0; i < PROF_COUNT; i++) tot += sProfHist[i];
        for (i = 0; i < PROF_COUNT && tot; i++) {
            PortDbgX(names[i], (unsigned)((u64)sProfHist[i] * 1000 / tot));
            sProfHist[i] = 0;
        }
    }
    sPerfGame = sPerfDl = sPerfSwap = sPerfPace = sPerfDlWait = 0;
    gPortPerfTris = gPortPerfDraws = 0;
}

static double sWalkMs = 20.0;   /* measured: the recording walk + its shown frame, ms */
static double sReplayMs = 4.0;  /* measured: one replay frame, ms */
static int sReplayN;            /* in-between frames this logic frame */
static unsigned sReplayBrokenCnt;
/* per report: updates by in-between frames chosen (0/1/2), and updates without the choice (rate 1 menus,
 * a capture or readback pending, right after a skipped frame) - the averages alone mix scene loads with
 * steady play */
static unsigned sReplayHist[3], sReplayNoChoice;
static unsigned sReplayDropped; /* in-between frames dropped because the update's first frame was late */
static u64 sPerfInterp;             /* ticks in in-between passes */
static u32 sInterpFrames;           /* in-between frames drawn */
static double sInterpElapsedSum;    /* logic time before the passes, ms (summed per report) */
/* PORT PERF (2026-10-03): the render thread (settings render_thread=1). The N64 drew frame N on the RCP while its CPU
 * ran the logic of frame N+1, and OoT is built for it: display lists live in double-buffered pools and the game waits
 * for the previous task before reusing one. The port did both on one core, one after the other. With the thread, the
 * game thread hands each update's display list over (RenderJob) and goes on; the render thread (the New 3DS's core 2,
 * the Old 3DS's core 1) walks it and draws the in-between frames. One update in flight: the next hand-over waits until
 * the renderer is idle, so a pool is never rebuilt while it is read. Requests the game makes while building a frame
 * are latched into its job (Port3ds_TakeFrameRequests); texture-cache invalidations from the game thread are queued
 * (gfx_pc.c); the game reads depth only from copies the renderer queued (gfx_3ds.c); audio is pumped only by the game
 * thread; HOME / sleep events run on the game thread while the renderer is idle. */
typedef struct {
    Gfx* dl;
    int skip, interp, chose, replayN, rate, skipOn;
    double base;      /* sLast when the update started (its frames' vblank slots) */
    void* capture;    /* latched game requests */
    int flat;
    float focusW;
} RenderJob;
static int sRenderThreadOn;      /* the thread runs */
static int sRenderThreadUse;     /* jobs go to it (render_thread_ab alternates this) */
static int sRenderStarted;       /* Port3ds_RenderThreadStart ran */
static int sRenderCore = 2;      /* the drawing thread's core (New 3DS) */
static volatile int sRenderPhase; /* where the render thread is (the watchdog in Port3ds_RenderWaitIdle logs it) */
static Thread sRenderThread;
static LightEvent sRenderGo, sRenderIdle;
static RenderJob sRenderJob;
static volatile int sRenderBusy;
static u64 sPerfRenderWait;      /* game thread ticks waiting for the renderer */
static void Port3ds_SetSlotAt(double base, int j, int frames, int rate);

int Port3ds_OnRenderThread(void) {
    return sRenderThreadOn && threadGetCurrent() == sRenderThread;
}
void Port3ds_RenderPhase(int phase) {
    sRenderPhase = phase;
}
int Port3ds_RenderPhaseGet(void) {
    return sRenderPhase;
}

static void Port3ds_RenderWaitIdle(void) {
    u64 t0;
    if (!sRenderThreadOn || !sRenderBusy) {
        return;
    }
    t0 = svcGetSystemTick();
    while (sRenderBusy) {
        /* audio keeps its retrace pace while the game waits for a slow frame (Old 3DS: the drawing core is capped);
         * watchdog: a renderer that does not come back within 2 s is logged once with where it is (sRenderPhase,
         * gfx_3ds.c phases), so a freeze on hardware leaves its place in boot.log */
        if (LightEvent_WaitTimeout(&sRenderIdle, 4000000LL) != 0 && sRenderBusy) {
            static int sLogged;
            Port3ds_MaybePumpAudio();
            if (!sLogged && svcGetSystemTick() - t0 > 2ull * SYSCLOCK_ARM11) {
                extern void PortDbgX(const char* label, unsigned val);
                extern void Port3ds_LogFlush(void);
                sLogged = 1;
                PortDbgX("[render] STUCK: the renderer has not finished for 2 s, phase", (unsigned)sRenderPhase);
                Port3ds_LogFlush();
            }
        }
    }
    sPerfRenderWait += svcGetSystemTick() - t0;
}

/* one update's drawing: the walk (the logic frame, or its first in-between frame) and the in-between frames */
static void Port3ds_RenderJob(const RenderJob* j) {
    extern u64 gPortGateTicksTotal;
    extern u64 gPortPerfGpuWait;
    extern void Port3ds_ApplyFrameRequests(void* capture, int flat, float focusW);
    const u64 gateA = gPortGateTicksTotal, waitA = gPortPerfGpuWait;
    const double kRetraceMs = 1000.0 / 59.83;
    u64 tA = svcGetSystemTick(), tB, tC;
    int replayN = j->replayN, i;
    Port3ds_ApplyFrameRequests(j->capture, j->flat, j->focusW);
    sRenderPhase = 1; /* job start */
    if (!j->skip) {
        extern void gfx_interp_pass(float t, int record);
        if (!j->interp) { /* the tag bookkeeping still resets every frame (gfx_pc.c interp_group) */
            gfx_interp_pass(1.0f, 0);
        } else {
            extern void gfx_interp_begin_frame(void);
            extern void gfx_replay_begin(void);
            extern int gPortReplayFirstK;
            gfx_interp_begin_frame();
            gfx_interp_pass(1.0f, 1);
            if (j->chose) {
                if (replayN == 0 && j->skipOn) {
                    /* PORT PERF (2026-10-02): no in-between frame fits (always the case on an Old 3DS: 0 of 300
                     * updates in every Old-3DS-speed report, hardware v30). Recording the frame for replays (the
                     * draw log, the t = 1/3 and 2/3 matrix stacks, the tagged-matrix matching) would be pure cost:
                     * draw it directly. The next frame then has nothing to blend from, which only matters on a
                     * console that can afford in-between frames - frame skip is off there. */
                    gfx_interp_pass(1.0f, 0);
                } else {
                    gfx_replay_begin();
                    gPortReplayFirstK = 2 - replayN; /* 2 extra: 1/3 first; 1: 2/3 first; 0: the logic frame */
                }
            }
        }
        {
            extern int gPortFramesFollow;
            gPortFramesFollow = (j->interp && replayN > 0) ? replayN : 0; /* gfx_3ds.c overlap: frames after the walk */
        }
        Port3ds_SetSlotAt(j->base, 0, (j->interp && replayN > 0) ? replayN + 1 : 1, j->rate);
        sRenderPhase = 2; /* frame start (stereo switch, C3D_FrameBegin) */
        gfx_start_frame();
        sRenderPhase = 3; /* the walk */
        gfx_run(j->dl);
    }
    tB = svcGetSystemTick();
    {
        /* the walk's waits (its C3D_FrameBegin, its present gate): not display-list cost */
        const u64 dlWait = (gPortPerfGpuWait >= waitA ? gPortPerfGpuWait - waitA : 0) + (gPortGateTicksTotal - gateA);
        sPerfDl += tB - tA;
        sPerfDlWait += dlWait < tB - tA ? dlWait : tB - tA;
    }
    if (sBench && !j->skip) {
        extern int gPortLegacyVbo;
        sBenchDl[gPortLegacyVbo] += tB - tA;
        sBenchFrames[gPortLegacyVbo]++;
    }
    if (!j->skip) {
        gfx_end_frame();
    }
    tC = svcGetSystemTick();
    if (j->interp && !j->skip) {
        extern int gPortReplayBroken;
        extern void gfx_replay_frame(int k);
        int rate = j->rate;
        double ms = (double)(tC - tA - (gPortGateTicksTotal - gateA)) / (SYSCLOCK_ARM11 / 1000.0);
        sWalkMs += (ms - sWalkMs) * 0.25;
        if (j->chose && rate == 3 && !j->skipOn && !sRenderThreadUse) {
            /* the logic frame's deadline: done on the GPU 0.5 ms before the second vblank of the update */
            extern float gPortGpuDrawMsAvg;
            double done = Port3ds_NowMs() - j->base + gPortGpuDrawMsAvg;
            sDeadlineMarginMs += ((2.0 * kRetraceMs - 0.5 - done) - sDeadlineMarginMs) * 0.2;
            if (sDeadlineMarginMs < 1.5) {
                sCoarseAuto = 1;
            } else if (sDeadlineMarginMs > 6.0) { /* coarse saves ~3 ms: no flip-flopping */
                sCoarseAuto = 0;
            }
            sWalkDoneSum += done;
            sWalkDoneN++;
        }
        if (replayN > 0 && gPortReplayBroken) {
            replayN = 0; /* drawn directly (an off-screen render, a full buffer): no in-between frames */
            sReplayBrokenCnt++;
        }
        {
            /* PORT PERF (2026-10-03): the update's frames must end on its third vblank, or the next update starts
             * a retrace late and the game slows (hardware v41: 18.5-19.2 updates/s, the logic frame ~30-33 ms
             * after the update's start against a ~33 ms deadline). The frame just submitted is the update's
             * first; each retrace it reaches the screen past the second vblank costs an in-between frame - the
             * t = 2/3 one first (the logic frame, t = 1, always shows) - instead of a retrace of game time. */
            extern int gPortPresentGate;
            extern double Port3ds_PredictShownMs(void);
            extern int Port3ds_FlipActive(void);
            double shown = Port3ds_PredictShownMs();
            if (gPortPresentGate && !Port3ds_FlipActive() && replayN > 0 && shown > 0.0 && rate == 3) {
                int late = (int)((shown - (j->base + 2.0 * kRetraceMs) + 1.5) / kRetraceMs);
                if (late > 0) {
                    sReplayDropped += late < replayN ? late : replayN;
                    replayN = late < replayN ? replayN - late : 0;
                }
            }
        }
        for (i = 0; i < replayN; i++) {
            u64 t0;
            extern int Port3ds_FlipActive(void);
            PROF_SET(PROF_PACE);
            if (Port3ds_FlipActive()) {
                Port3ds_SetSlotAt(j->base, i + 1, replayN + 1, rate); /* drawn now, shown at its vblank */
            } else {
                Port3ds_WaitUntil(j->base + (rate * kRetraceMs) * (i + 1) / (replayN + 1));
            }
            u64 gateR = gPortGateTicksTotal;
            t0 = svcGetSystemTick();
            { extern int gPortFramesFollow; gPortFramesFollow = replayN - 1 - i; }
            sRenderPhase = 10 + i; /* in-between frame i */
            gfx_replay_frame(2 - replayN + 1 + i);
            gfx_end_frame();
            ms = (double)(svcGetSystemTick() - t0 - (gPortGateTicksTotal - gateR)) / (SYSCLOCK_ARM11 / 1000.0);
            sReplayMs += (ms - sReplayMs) * 0.25;
            sPerfInterp += svcGetSystemTick() - t0;
            sInterpFrames++;
        }
    }
    {
        extern void Port3ds_EndUpdateFrames(void);
        extern int gPortFramesFollow;
        gPortFramesFollow = 0;
        sRenderPhase = 20; /* closing the update */
        Port3ds_EndUpdateFrames(); /* gfx_3ds.c overlap: a citro3d frame still open (planned frames not drawn) ends */
    }
    sRenderPhase = 0;
    sPerfSwap += svcGetSystemTick() - tB;
}

static void Port3ds_RenderThreadMain(void* arg) {
    (void)arg;
    for (;;) {
        RenderJob job;
        LightEvent_Wait(&sRenderGo);
        if (!sRenderBusy) {
            continue;
        }
        __sync_synchronize();
        job = sRenderJob;
        Port3ds_RenderJob(&job);
        __sync_synchronize();
        sRenderBusy = 0;
        LightEvent_Signal(&sRenderIdle);
    }
}

static void Port3ds_RenderThreadStart(void) {
    extern void PortDbg(const char* s);
    extern void PortDbgX(const char* label, unsigned val);
    extern int gPortRenderThreaded, gPortEventsExternal;
    extern int Port3ds_FlipActive(void);
    s32 prio = 0x30;
    bool n3ds = false;
    if (!(sRenderThreadSetting || sRenderThreadAB) || sRenderStarted || !Port3ds_FlipActive()) {
        return; /* (the present gate's waits pump audio: the thread needs the flip presenter) */
    }
    sRenderStarted = 1;
    APT_CheckNew3DS(&n3ds);
    (void)n3ds;
    LightEvent_Init(&sRenderGo, RESET_ONESHOT);
    LightEvent_Init(&sRenderIdle, RESET_ONESHOT);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* the main thread's stack size (port/oot.rsf StackSize): the walk recurses through nested display lists */
    sRenderThread = threadCreate(Port3ds_RenderThreadMain, NULL, 256 * 1024, prio, sRenderCore, false);
    if (sRenderThread == NULL) {
        PortDbg("[render] thread: not created, drawing on the game thread");
        return;
    }
    sRenderThreadOn = 1;
    sRenderThreadUse = 1;
    gPortRenderThreaded = 1;
    gPortEventsExternal = 1;
    PortDbgX("[render] thread on core", (unsigned)sRenderCore);
}

void PortGfx_RunTask(OSTask* task) {
    u64 tA = svcGetSystemTick(), tC, tD;
#ifdef PORT_MM_NODRAW /* (bring-up: run the game without drawing, to isolate GPU faults) */
    { extern void PortDbgX(const char* label, unsigned val); static unsigned sN; if (sN++ < 3) PortDbgX("[nodraw] frame", sN); }
    return;
#endif
#ifdef PORT_LOG_SYNC
    { extern void PortDbgX(const char* label, unsigned val); static unsigned sF; if (sF < 40) PortDbgX("[gfx] frame", ++sF); }
#endif
    PROF_SET(PROF_INPUT);
    if (sPerfLastEnd != 0) sPerfGame += tA - sPerfLastEnd;
    Port3ds_RenderWaitIdle(); /* one update in flight (render thread) */
    if (!sGfxInited) {
        PortGfx_Init();
    }
    if ((sRenderThreadSetting || sRenderThreadAB) && !sRenderStarted && sGfxInited) {
        Port3ds_RenderThreadStart(); /* after the first frames: graphics and the flip presenter are up */
    }
    if (sRenderThreadUse) {
        extern void gfx_handle_events(void);
        gfx_handle_events(); /* HOME / sleep / exit: here, while the renderer is idle */
        {
            extern volatile int gPortAptResumed;
            if (gPortAptResumed) { /* (diagnostic: the game loop runs again after HOME / sleep) */
                extern void PortDbg(const char* s);
                extern void Port3ds_LogFlush(void);
                gPortAptResumed = 0;
                PortDbg("[apt] the game runs again");
                Port3ds_LogFlush();
            }
        }
    }
    Port3ds_PollInput();
    { extern volatile unsigned gPortGameBeat; gPortGameBeat++; } /* (the freeze detector: the game loop advanced) */
    { extern void Port3ds_PumpInput(void); Port3ds_PumpInput(); } /* live buttons -> game PadMgr */
    /* PORT MM TODO (audio bring-up): OoT needed Audio_PortEnsureNullChannels() here (an OoT game-side patch) */
    /* PORT (2026-10-01): frame skip. On the N64 a frame that takes too long slows the game down (lag). The
     * Old 3DS spends about 80% of an update drawing it (hardware profile: ~98 ms per update, the N64 takes
     * 50 ms), so the game ran at about half speed. While the game is more than half an update behind
     * schedule, an update is not drawn: its logic and audio still run and the screen keeps the previous
     * frame. Never two in a row (at least 10 frames per second), never a frame the game reads back (pause
     * background, transitions), and no in-between frames right after a skip (the positions they blend from
     * would be two updates old). */
    int skip = 0, afterSkip = sSkippedLast;
    {
        extern int gPortO3dsSim;
        extern int Port3ds_InterpBlocked(void);
        static int sN3ds = -1;
        if (sN3ds < 0) {
            bool n3ds = false;
            APT_CheckNew3DS(&n3ds);
            sN3ds = n3ds;
        }
        sSkipOn = sFrameSkip > 0 || (sFrameSkip < 0 && (!sN3ds || gPortO3dsSim));
        {
            /* PORT PERF (2026-10-02): coarser N64-shading splits while CPU-bound (gfx_pc.c) */
            extern void gfx_split_thresholds(int coarse);
            static int sCoarse = -1;
            int coarse = sSkipOn || (sSplitAuto && sCoarseAuto);
            if (!sSplitSet && sCoarse != coarse) {
                gfx_split_thresholds(coarse);
                sCoarse = coarse;
            }
            sCoarseUpdates += !sSplitSet && coarse;
            { extern int gPortRawRelax; gPortRawRelax = sSpeedRules == 2 || (sSpeedRules == 1 && sSkipOn); } /* (gfx_pc.c) */
        }
        skip = sSkipOn && !sSkippedLast && sBehindMs > 0.5 * Port3ds_UpdateRate() * (1000.0 / 59.83) &&
               !Port3ds_InterpBlocked();
        sSkippedLast = skip;
        sSkipCount += skip;
    }
    if (sBench) {
        extern int gPortLegacyVbo;
        static unsigned sBenchTick;
        gPortLegacyVbo = (++sBenchTick) & 1;
    }
    RenderJob job;
    memset(&job, 0, sizeof(job));
    job.dl = (Gfx*)task->t.data_ptr;
    job.skip = skip;
    job.interp = sInterp;
    job.rate = Port3ds_UpdateRate();
    job.base = sLast;
    job.skipOn = sSkipOn;
    if (sInterp && !skip) {
        /* PORT (2026-09-30): 60 fps. The logic runs at 20/s (R_UPDATE_RATE retraces per update). The
         * display-list walk RECORDS the frame (gfx_pc.c "replay"), with every tagged matrix also evaluated
         * at t = 1/3 and 2/3 between the previous logic frame and this one (port_interp.h tags, the
         * Zelda64Recomp / Ship of Harkinian model); then each retrace shows a REPLAY - only the vertex
         * positions are recomputed and the recorded draws re-issued (a few ms instead of a whole walk).
         * As many in-between frames as fit (measured costs), so a slow scene/console drops to fewer and
         * the logic keeps its N64 speed. The world is shown one logic frame (50 ms) late, as in SoH. */
        extern int Port3ds_InterpBlocked(void);
        const double kRetraceMs = 1000.0 / 59.83;
        int rate = job.rate;
        sReplayN = 0;
        sChose = 0;
        if (!(sLast != 0.0 && rate > 1 && !sBench && !Port3ds_InterpBlocked() && !afterSkip)) {
            sReplayNoChoice++;
        } else {
            /* with the render thread the logic runs beside the drawing: the whole update is the drawing's budget */
            double budget = rate * kRetraceMs, elapsed = sRenderThreadUse ? 2.0 : Port3ds_NowMs() - sLast;
            sChose = 1;
            for (sReplayN = rate - 1; sReplayN > 0 && sInterp < 2; sReplayN--) {
                if (elapsed + sWalkMs + sReplayN * sReplayMs <= budget) break;
            }
            if (sReplayN > 2) sReplayN = 2; /* t = 1/3, 2/3 */
            sReplayHist[sReplayN]++;
            sInterpElapsedSum += elapsed;
        }
        job.chose = sChose;
        job.replayN = sReplayN;
    }
    {
        extern void Port3ds_TakeFrameRequests(void** capture, int* flat, float* focusW);
        Port3ds_TakeFrameRequests(&job.capture, &job.flat, &job.focusW);
    }
    gPortInterpOn = sInterp && !skip && job.replayN > 0; /* for the NEXT update's actor drawing (z_actor.c) */
    if (sRenderThreadUse) {
        /* frames the game reads back (pause background, statediff) are drawn before the game goes on */
        extern int Port3ds_DrawIdActive(void);
        int wait = job.capture != NULL || Port3ds_DrawIdActive();
        sRenderJob = job;
        __sync_synchronize();
        sRenderBusy = 1;
        LightEvent_Signal(&sRenderGo);
        if (wait) {
            Port3ds_RenderWaitIdle();
        }
    } else {
        Port3ds_RenderJob(&job);
    }
    tC = svcGetSystemTick();
    PROF_SET(PROF_PACE);
    Port3ds_PaceFrame();
    PROF_SET(PROF_GAME);
    tD = svcGetSystemTick();
    sPerfPace += tD - tC;
    sPerfLastEnd = tD;
    /* Frame-rate log: game updates per second measured on the wall clock (x10), every 300 frames.
     * OoT's logic is designed for 20/s (R_UPDATE_RATE=3 VI retraces per update at 60 Hz). */
    { static u64 t0 = 0; static unsigned n = 0;
      if (t0 == 0) t0 = osGetTime();
      if (++n == 300) { u64 t1 = osGetTime();
          Port3ds_RenderWaitIdle(); /* the switches below may touch the renderer */
          extern void PortDbgX(const char*, unsigned); extern void* gRegEditor;
          /* PORT (2026-10-01): the report goes to boot.log only while a measurement switch is on (prof,
           * perf_ab, gpu_ab, aa_ab): ~50 lines every 15 s, each flushed to the SD card, otherwise */
          { extern int gPortPerfAB; sLogMute = !(sProfOn || gPortPerfAB || sGpuAB || sAaAB || sCmdflushAB || sPresentAB || sOverlapAB ||
                                                     sRawAB || sRenderThreadAB || sAudioShareAB || sReplayCopyAB || sSpeedRulesAB); }
          PortDbgX("perf updates/s x10", (unsigned)(3000000ull / (t1 - t0 ? t1 - t0 : 1)));
          PortDbgX("perf frames shown/s x10 (60fps interp)",
                   (unsigned)((300ull - sSkipCount + sInterpFrames) * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
          { /* memory over a long session (leak check): linear heap (GPU buffers, textures) and the app heap */
              extern u32 linearSpaceFree(void);
              struct mallinfo mi = mallinfo();
              PortDbgX("perf mem linear free KB", linearSpaceFree() / 1024);
              PortDbgX("perf mem heap used KB", (unsigned)mi.uordblks / 1024);
              PortDbgX("perf mem heap free KB", (unsigned)mi.fordblks / 1024);
          }
          {
              extern u64 gPortPerfFrameEnd;
              extern float gPortPerfGpuProcMs, gPortPerfGpuDrawMs;
              extern unsigned gPortPerfGpuFrames;
              unsigned nf = gPortPerfGpuFrames ? gPortPerfGpuFrames : 1;
              PortDbgX("perf us/GPU frame C3D_FrameEnd (cpu)", (unsigned)(gPortPerfFrameEnd / nf / (SYSCLOCK_ARM11 / 1000000)));
              PortDbgX("perf us/GPU frame cpu FrameBegin..End", (unsigned)(gPortPerfGpuProcMs * 1000.0f / nf));
              PortDbgX("perf us/GPU frame gpu drawing (hw only)", (unsigned)(gPortPerfGpuDrawMs * 1000.0f / nf));
              PortDbgX("perf GPU frames (of 300 updates)", gPortPerfGpuFrames);
              gPortPerfFrameEnd = 0;
              gPortPerfGpuProcMs = gPortPerfGpuDrawMs = 0.0f;
              gPortPerfGpuFrames = 0;
          }
          { extern int gPortCmdlistFlush; PortDbgX("perf cmdlist_flush", (unsigned)gPortCmdlistFlush); }
          { /* gfx_3ds.c presentation model: frames that reached the screen (est.) and the gate's waiting */
              extern int gPortPresentGate;
              extern unsigned gPortPerfReplaced, gPortPerfPresented;
              extern u64 gPortPerfGateWait;
              unsigned shown = gPortPerfPresented - (gPortPerfReplaced < gPortPerfPresented ? gPortPerfReplaced : 0);
              PortDbgX("perf present gate", (unsigned)gPortPresentGate);
              { /* the 3D slider doubles draws and GPU work: say which mode a report measured */
                  extern unsigned gPortPerfStereoFrames;
                  PortDbgX("perf frames drawn in 3D (of presented)", gPortPerfStereoFrames);
                  PortDbgX("perf frames presented", gPortPerfPresented);
                  gPortPerfStereoFrames = 0;
              }
              PortDbgX("perf us logic frame done on GPU after update start", sWalkDoneN ? (unsigned)(sWalkDoneSum * 1000.0 / sWalkDoneN) : 0);
              PortDbgX("perf updates with coarse split (of 300)", sCoarseUpdates);
              sWalkDoneSum = 0.0;
              sWalkDoneN = sCoarseUpdates = 0;
              PortDbgX("perf frames displayed/s x10 (est)", (unsigned)((u64)shown * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
              PortDbgX("perf frames replaced before shown", gPortPerfReplaced);
              PortDbgX("perf us/update present gate wait", (unsigned)(gPortPerfGateWait / 300 / (SYSCLOCK_ARM11 / 1000000)));
              gPortPerfReplaced = gPortPerfPresented = 0;
              gPortPerfGateWait = 0;
          }
          PortDbgX("perf frames skipped (of 300 updates)", sSkipCount);
          PortDbgX("perf updates late >1 retrace (of 300)", sLateUpdates);
          PortDbgX("perf updates with 2 in-between frames", sReplayHist[2]);
          PortDbgX("perf updates with 1 in-between frame", sReplayHist[1]);
          PortDbgX("perf updates with 0 in-between (budget)", sReplayHist[0]);
          PortDbgX("perf updates without interp choice", sReplayNoChoice);
          PortDbgX("perf in-between frames dropped (first frame late)", sReplayDropped);
          { /* gfx_3ds.c flip presenter: exact counts */
              extern unsigned gPortPerfFlipShown, gPortPerfFlipSkipped, gPortPerfFlipRepeats;
              extern int Port3ds_FlipActive(void);
              PortDbgX("perf flip presenter", (unsigned)Port3ds_FlipActive());
              { extern void Port3ds_FlipDump(void); Port3ds_FlipDump(); }
              PortDbgX("perf flip frames shown/s x10", (unsigned)((u64)gPortPerfFlipShown * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
              PortDbgX("perf flip frames skipped (late)", gPortPerfFlipSkipped);
              PortDbgX("perf flip refreshes repeating a frame", gPortPerfFlipRepeats);
              gPortPerfFlipShown = gPortPerfFlipSkipped = gPortPerfFlipRepeats = 0;
          }
          { /* gfx_3ds.c CPU/GPU overlap: frames that handed the GPU over without ending the citro3d frame */
              extern int gPortOverlap;
              extern u32 gPortPerfOverlapFrames;
              extern float gPortPerfCmdBufMax;
              PortDbgX("perf render thread", (unsigned)sRenderThreadUse);
              PortDbgX("perf us/update game waits for the renderer", (unsigned)(sPerfRenderWait / 300 / (SYSCLOCK_ARM11 / 1000000)));
              sPerfRenderWait = 0;
              PortDbgX("perf overlap on", (unsigned)gPortOverlap);
              PortDbgX("perf overlap frames (of 300 updates)", gPortPerfOverlapFrames);
              PortDbgX("perf cmdbuf peak use % (1 MB)", (unsigned)(gPortPerfCmdBufMax * 100.0f));
              {
                  extern u64 gPortPerfDepthWait;
                  extern u32 gPortPerfDepthAsync;
                  PortDbgX("perf queued depth copies read", gPortPerfDepthAsync);
                  PortDbgX("perf us/read waiting for a queued depth copy",
                           gPortPerfDepthAsync ? (unsigned)(gPortPerfDepthWait / gPortPerfDepthAsync / (SYSCLOCK_ARM11 / 1000000)) : 0);
                  gPortPerfDepthWait = 0;
                  gPortPerfDepthAsync = 0;
              }
              gPortPerfOverlapFrames = 0;
              gPortPerfCmdBufMax = 0.0f;
          }
          sReplayDropped = 0;
          sLateUpdates = sReplayNoChoice = 0;
          sReplayHist[0] = sReplayHist[1] = sReplayHist[2] = 0;
          PortDbgX("perf frame skip on", (unsigned)sSkipOn);
          { extern int gPortShadeLinear; PortDbgX("perf shade linear (GPU path)", (unsigned)gPortShadeLinear); }
          sSkipCount = 0;
          { extern uint32_t gPortInterpMatched, gPortInterpMissed;
            PortDbgX("perf interp mtx matched/frame", sInterpFrames ? gPortInterpMatched / sInterpFrames : 0);
            PortDbgX("perf interp mtx missed/frame", sInterpFrames ? gPortInterpMissed / sInterpFrames : 0);
            PortDbgX("perf us/replay frame", sInterpFrames ? (unsigned)(sPerfInterp / sInterpFrames / (SYSCLOCK_ARM11 / 1000000)) : 0);
            PortDbgX("perf us/walk+first frame (est)", (unsigned)(sWalkMs * 1000.0));
            PortDbgX("perf us/replay frame (est)", (unsigned)(sReplayMs * 1000.0));
            PortDbgX("perf replay fallbacks (direct frames)", sReplayBrokenCnt);
            sReplayBrokenCnt = 0;
            PortDbgX("perf us/frame logic before passes", (unsigned)(sInterpElapsedSum * 1000.0 / n));
            sInterpElapsedSum = 0;
            { extern uint32_t gPortInterpSame, gPortInterpJump;
              PortDbgX("perf interp mtx identical/frame", sInterpFrames ? gPortInterpSame / sInterpFrames : 0);
              PortDbgX("perf interp mtx jump (not blended)/frame", sInterpFrames ? gPortInterpJump / sInterpFrames : 0);
              gPortInterpSame = gPortInterpJump = 0; }
            { extern uint32_t gPortInterpVtx, gPortInterpVtxMiss;
              PortDbgX("perf interp skin vtx blended/frame", sInterpFrames ? gPortInterpVtx / sInterpFrames : 0);
              PortDbgX("perf interp skin vtx loads unmatched/frame", sInterpFrames ? gPortInterpVtxMiss / sInterpFrames : 0);
              gPortInterpVtx = gPortInterpVtxMiss = 0; }
            gPortInterpMatched = gPortInterpMissed = 0; }
          sInterpFrames = 0; sPerfInterp = 0;
          if (gRegEditor) PortDbgX("perf R_UPDATE_RATE", (unsigned)*(short*)((char*)gRegEditor + 0x14 + 126 * 2));
          PortDbgX("perf audio pumps/s x10", (unsigned)((u64)sPortAudioPumps * 10000ull / (t1 - t0 ? t1 - t0 : 1)));
          { extern u32 gPortPerfCacheFallbacks; PortDbgX("perf cache ops through the GPU service (fallbacks)", gPortPerfCacheFallbacks); gPortPerfCacheFallbacks = 0; }
          { extern unsigned gPortAudioCore1Limit; extern int gPortAudioCoreNow;
            PortDbgX("perf audio mixer system-core share %", gPortAudioCore1Limit);
            PortDbgX("perf audio mixer core", (unsigned)gPortAudioCoreNow); }
          sPortAudioPumps = 0;
          Port3ds_PerfReport(n);
          if (!sLogMute) Port3ds_MemProbe();
          if (sBench && sBenchFrames[0] && sBenchFrames[1]) {
              PortDbgX("bench us/frame INDEXED", (unsigned)(sBenchDl[0] / sBenchFrames[0] / (SYSCLOCK_ARM11 / 1000000)));
              PortDbgX("bench us/frame ARRAY", (unsigned)(sBenchDl[1] / sBenchFrames[1] / (SYSCLOCK_ARM11 / 1000000)));
              sBenchDl[0] = sBenchDl[1] = 0;
              sBenchFrames[0] = sBenchFrames[1] = 0;
          }
          { /* perf_ab=1: alternate New 3DS speed / Old 3DS approximation every 4 reports (~1 min) so one
             * play session measures both (docs/3ds-60fps-plan.md P0) */
              extern int gPortPerfAB, gPortO3dsSim;
              static unsigned sReports;
              bool n3ds = false;
              APT_CheckNew3DS(&n3ds);
              PortDbgX(gPortO3dsSim ? "perf mode O3DS-sim (268MHz no L2)" : "perf mode N3DS (804MHz L2)", 1);
              { extern int gPortGpuVtx; PortDbgX(gPortGpuVtx ? "perf vertex path GPU" : "perf vertex path CPU", 1); }
              { extern int gPortAA; PortDbgX("perf anti-aliasing", (unsigned)gPortAA); }
              static unsigned sPgReports;
              /* every 4 reports: with cmdflush_ab (every 2) one session covers all four combinations */
              if (sPresentAB && (++sPgReports % 4) == 0) { /* applied from the next frame */
                  extern int gPortPresentGate;
                  gPortPresentGate = !gPortPresentGate;
              }
              static unsigned sRtReports;
              if (sRenderThreadAB && sRenderThreadOn && (++sRtReports % 2) == 0) { /* the renderer is idle here */
                  extern int gPortRenderThreaded, gPortEventsExternal;
                  sRenderThreadUse = !sRenderThreadUse;
                  gPortRenderThreaded = gPortEventsExternal = sRenderThreadUse;
              }
              static unsigned sOvReports;
              if (sOverlapAB && (++sOvReports % 2) == 0) { /* from the next update (gfx_3ds.c reads it per frame) */
                  extern int gPortOverlap;
                  gPortOverlap = !gPortOverlap;
              }
              static unsigned sCfReports;
              if (sCmdflushAB && (++sCfReports % 2) == 0) { /* applied from the next C3D_FrameEnd */
                  extern int gPortCmdlistFlush;
                  gPortCmdlistFlush = !gPortCmdlistFlush;
              }
              static unsigned sAaReports;
              if (sAaAB && (++sAaReports % 2) == 0) { /* applied before the next frame (gfx_3ds.c) */
                  extern int gPortAA;
                  gPortAA = !gPortAA;
              }
              static unsigned sShareReports;
              if (sAudioShareAB && (++sShareReports % 2) == 0) { /* (only while the mixer is on core 1) */
                  extern void Port3ds_AudioSetShare(int percent);
                  extern unsigned gPortAudioCore1Limit;
                  Port3ds_AudioSetShare(gPortAudioCore1Limit == 30 ? 55 : gPortAudioCore1Limit == 55 ? 80 : 30);
              }
              static unsigned sRawReports;
              if (sRawAB && (++sRawReports % 2) == 0) { /* gfx_start_frame latches it for the next frame */
                  extern int gPortRawVtxWant;
                  gPortRawVtxWant = !gPortRawVtxWant;
              }
              static unsigned sSpeedReports;
              if (sSpeedRulesAB && (++sSpeedReports % 2) == 0) { /* latched per update (gPortRawRelax) */
                  sSpeedRules = sSpeedRules ? 0 : 1;
              }
              static unsigned sCopyReports;
              if (sReplayCopyAB && (++sCopyReports % 2) == 0) { /* read when a recording ends: next update */
                  extern int gPortReplayCopy;
                  gPortReplayCopy = !gPortReplayCopy;
              }
              static unsigned sGpuReports;
              if (sGpuAB && (++sGpuReports % 2) == 0) { /* between frames: takes effect from the next one */
                  extern int gPortGpuVtx;
                  extern void gfx_citro3d_set_gpu_mode(int gpu);
                  gfx_citro3d_set_gpu_mode(!gPortGpuVtx);
              }
              if (gPortPerfAB && n3ds && (++sReports % 4) == 0) {
                  gPortO3dsSim = !gPortO3dsSim;
                  osSetSpeedupEnable(!gPortO3dsSim);
                  if (sRenderThreadOn && !sRenderThreadAB) {
                      /* PORT (2026-10-04): the Old 3DS has no core 2 - its phases draw on the game thread, as an
                       * Old 3DS does (the renderer is idle here) */
                      extern int gPortRenderThreaded, gPortEventsExternal;
                      sRenderThreadUse = !gPortO3dsSim;
                      gPortRenderThreaded = gPortEventsExternal = sRenderThreadUse;
                  }
              }
          }
          sLogMute = 0;
          n = 0; t0 = t1; } }
}

#define ROM_PATH "sdmc:/3ds/mm/baserom-decompressed.z64"
#define LOG_PATH "sdmc:/3ds/mm/boot.log"
int gPortMmAudio;

static void boot_flush(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* PORT PERF (2026-09-18): logging must be CHEAP — it is called from engine paths that run
 * every frame. The old Log() did gspWaitForVBlank() (a ~16ms hardware wait) AND fopen/fclose
 * on the SD on EVERY call, so per-frame logging stalled the game to ~1fps (the roadmap's
 * "never ship per-frame logging" trap). Now: keep one file handle open, fwrite+fflush the
 * line (crash still leaves the last line on the card), and print to the console — no vblank
 * wait, no reopen. The bottom-screen console updates on the graph loop's own swap. */
static FILE* sLogFile = NULL;
/* PORT PERF (2026-10-03): once the game runs, log lines are written by a low-priority thread. Each line used
 * to be flushed to the SD card on the spot; the perf report (~60 lines every 300 updates while a measurement
 * switch is on) stalled the game for up to half a second each time (hardware v42: one update a retrace late in
 * every report, felt as a pause "out of nowhere"). Boot messages stay synchronous: a crash during startup still
 * leaves its last line in boot.log. */
#define LOG_BUF 16384
static char sLogBuf[2][LOG_BUF];
static int sLogLen, sLogCur;
static LightLock sLogLock;
static LightEvent sLogEvent;
static Thread sLogThread;
static volatile int sLogAsync;

static void Log_WriterThread(void* arg) {
    (void)arg;
    for (;;) {
        char* buf;
        int len;
        LightEvent_Wait(&sLogEvent);
        LightLock_Lock(&sLogLock);
        buf = sLogBuf[sLogCur];
        len = sLogLen;
        sLogCur ^= 1; /* the game appends to the other buffer meanwhile */
        sLogLen = 0;
        LightLock_Unlock(&sLogLock);
        if (len > 0) {
            if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
            if (sLogFile) {
                fwrite(buf, 1, (size_t)len, sLogFile);
                fflush(sLogFile);
            }
        }
    }
}

static void Log_StartAsync(void) {
    s32 prio = 0x30;
    LightLock_Init(&sLogLock);
    LightEvent_Init(&sLogEvent, RESET_ONESHOT);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sLogThread = threadCreate(Log_WriterThread, NULL, 8 * 1024, 0x3F, -2, true); /* lowest priority, same core */
    sLogAsync = sLogThread != NULL;
    (void)prio;
}

/* append one line for the writer thread (sLogAsync) */
static void Log_Append(const char* s) {
    int n = (int)strlen(s);
    LightLock_Lock(&sLogLock);
    if (sLogLen + n + 1 <= LOG_BUF) { /* full (the writer far behind): the line is dropped */
        memcpy(sLogBuf[sLogCur] + sLogLen, s, (size_t)n);
        sLogBuf[sLogCur][sLogLen + n] = '\n';
        sLogLen += n + 1;
    }
    LightLock_Unlock(&sLogLock);
    LightEvent_Signal(&sLogEvent);
}

/* PORT (2026-11-12): the freeze detector (a 2DS froze on a sword swing; boot.log just stopped, its last lines still in
 * the buffer: the writer thread above runs at the lowest priority on the game's core). A thread on core 1 checks once a
 * second that the game loop still advances (gPortGameBeat, once per update in the game loop). After 4 s without it, it
 * writes boot.log itself - the buffered lines, then where things stand: the game thread's stage (port_prof.h PROF_*),
 * the render thread's phase, a pending audio task, the last ROM read. Once per freeze. (HOME / sleep also stop the
 * loop: such an entry after them means nothing.) */
volatile unsigned gPortGameBeat;
static void FreezeHex(FILE* f, const char* label, unsigned val) {
    static const char hx[] = "0123456789abcdef";
    char b[12];
    int i;
    fputs(label, f);
    b[0] = '=', b[1] = '0', b[2] = 'x';
    for (i = 0; i < 8; i++) b[3 + i] = hx[(val >> (28 - 4 * i)) & 0xF];
    b[11] = '\0';
    fputs(b, f);
    fputc('\n', f);
}
static volatile int sAptAway; /* in the HOME menu / asleep: the game is paused on purpose (Port3ds_CpuStateHook) */
/* PORT (2026-11-14): HOME pressed (set by the APT suspend hook, cleared on restore / wake-up). gfx_3ds.c's flip presenter
 * presents nothing while it is set. gPortAptStep: how far the switch to the HOME menu got - 1 the APT hooks ran (next:
 * VRAM save, the screen capture handed to the HOME menu), 2 the DSP is being put to sleep (audio_3ds.c Port3ds_DspHook);
 * the switch is done when the GPU right is given up. */
volatile int gPortAptSuspended, gPortAptStep;
static void Port3ds_FreezeWatch(void* arg) {
    extern volatile unsigned char gPortProf;
    extern unsigned gPortLastDma[4];
    extern int Port3ds_AudioJobPending(void);
    unsigned last = gPortGameBeat;
    int still = 0, logged = 0, homeStill = 0;
    (void)arg;
    for (;;) {
        FILE* f;
        svcSleepThread(1000000000LL);
        if (gPortAptSuspended && gspHasGpuRight()) { /* HOME pressed, and the HOME menu has not got the screen yet */
            if (++homeStill == 4 && (f = fopen(LOG_PATH, "a")) != NULL) {
                if (sLogAsync && LightLock_TryLock(&sLogLock) == 0) {
                    if (sLogLen > 0) {
                        fwrite(sLogBuf[sLogCur], 1, (size_t)sLogLen, f);
                        sLogLen = 0;
                    }
                    LightLock_Unlock(&sLogLock);
                }
                fputs("*** STUCK: 4 s after HOME, the HOME menu still has not taken over ***\n", f);
                FreezeHex(f, "  step (1 APT hooks done, 2 DSP going to sleep)", (unsigned)gPortAptStep);
                FreezeHex(f, "  render thread phase", (unsigned)Port3ds_RenderPhaseGet());
                FreezeHex(f, "  audio task still running", (unsigned)Port3ds_AudioJobPending());
                fclose(f);
            }
            continue;
        }
        homeStill = 0;
        if (gPortGameBeat != last || sAptAway) {
            last = gPortGameBeat;
            still = 0;
            logged = 0;
            continue;
        }
        if (++still < 4 || logged) {
            continue;
        }
        logged = 1;
        f = fopen(LOG_PATH, "a");
        if (f == NULL) {
            continue;
        }
        if (sLogAsync && LightLock_TryLock(&sLogLock) == 0) { /* (the lines the writer never got to) */
            if (sLogLen > 0) {
                fwrite(sLogBuf[sLogCur], 1, (size_t)sLogLen, f);
                sLogLen = 0;
            }
            LightLock_Unlock(&sLogLock);
        }
        fputs("*** FREEZE: the game has not advanced for 4 s ***\n", f);
        FreezeHex(f, "  game updates so far", last);
        FreezeHex(f, "  game thread stage (port_prof.h PROF_*)", gPortProf);
        FreezeHex(f, "  render thread phase", (unsigned)Port3ds_RenderPhaseGet());
        FreezeHex(f, "  audio task still running", (unsigned)Port3ds_AudioJobPending());
        FreezeHex(f, "  last ROM read: vrom", gPortLastDma[0]);
        FreezeHex(f, "  last ROM read: size", gPortLastDma[2]);
        fclose(f);
    }
}
static void Port3ds_FreezeWatchStart(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* core 1: a game thread spinning on core 0 can't starve it (Old 3DS: the system core, already shared with audio) */
    if (threadCreate(Port3ds_FreezeWatch, NULL, 8 * 1024, prio > 0x19 ? prio - 2 : prio, 1, true) == NULL) {
        threadCreate(Port3ds_FreezeWatch, NULL, 8 * 1024, prio > 0x19 ? prio - 2 : prio, -2, true);
    }
}

/* before exit(): what the writer has not written yet (gfx_3ds.c HOME -> Close) */
void Port3ds_LogFlush(void) {
    if (!sLogAsync) return;
    LightLock_Lock(&sLogLock);
    if (sLogLen > 0) {
        if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
        if (sLogFile) {
            fwrite(sLogBuf[sLogCur], 1, (size_t)sLogLen, sLogFile);
            fflush(sLogFile);
        }
        sLogLen = 0;
    }
    LightLock_Unlock(&sLogLock);
}

static void Log(const char* s) {
    /* PORT (2026-09-20): an engine path calls the logger with an empty string ~15x/frame,
     * which flooded boot.log with bare '\n' (millions of lines / ~18 MB per session — real
     * SD-write load on hardware, the roadmap's "never ship per-frame logging" trap). Empty
     * lines carry no information, so drop them here at the chokepoint. */
    if (s == NULL || s[0] == '\0' || sLogMute) return;
    if (!sTouchUi) printf("%s\n", s); /* the bottom screen is the touch panel once it is up */
    if (sLogAsync) {
        Log_Append(s);
        return;
    }
    if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
    if (sLogFile) { fputs(s, sLogFile); fputc('\n', sLogFile); fflush(sLogFile); }
}

/* checkpoint logger called from engine init (main.c) */
void PortDbg(const char* s) { Log(s); }

/* PORT (2026-11-12): the game's fatal errors (src/boot/fault.c Fault_AddHungupAndCrashImpl: "HungUp <file>:<line>" and
 * the like) are written to boot.log, then the program stops with a real break - Luma3DS shows its exception screen */
void PortDbgX(const char* label, unsigned val);
static void Port3ds_FatalTail(void* caller) __attribute__((noreturn));
void Port3ds_Fatal(const char* exp1, const char* exp2, void* caller) {
    sLogMute = 0; /* (never muted: this is the line that matters) */
    Log("*** The game stopped with an error: ***");
    Log((exp1 != NULL && exp1[0] != '\0') ? exp1 : "(no message)");
    if (exp2 != NULL) {
        Log(exp2);
    }
    Port3ds_FatalTail(caller);
}
/* fault.c Fault_AddHungupAndCrash: the file and line as given (the port's sprintf left its message empty) */
void Port3ds_FatalAt(const char* file, s32 line, void* caller) {
    char b[160];
    int n = 0, k;
    char d[12];
    unsigned v = line < 0 ? 0u : (unsigned)line;
    sLogMute = 0;
    Log("*** The game stopped with an error (HungUp): ***");
    /* "<file>:<line>", put together by hand */
    if (file != NULL) {
        while (file[n] != '\0' && n < 140) {
            b[n] = file[n];
            n++;
        }
    }
    b[n++] = ':';
    k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && k < 11);
    while (k > 0) {
        b[n++] = d[--k];
    }
    b[n] = '\0';
    Log(b);
    Port3ds_FatalTail(caller);
}
unsigned gPortLastDma[4]; /* the last ROM read: vrom, RAM, size, and how many so far (port/src/dma_shim.c) */
static void Port3ds_FatalTail(void* caller) {
    PortDbgX("  called from (look up in mm.elf)", (unsigned)(uintptr_t)caller);
    PortDbgX("  last ROM request: vrom", gPortLastDma[0]);
    PortDbgX("  last ROM request: to RAM", gPortLastDma[1]);
    PortDbgX("  last ROM request: size", gPortLastDma[2]);
    PortDbgX("  ROM reads so far", gPortLastDma[3]);
    { /* the memory left at that moment (an Old 3DS / 2DS has far less) */
        extern unsigned gPortDbgRealloc[4];
        PortDbgX("  linear space free", (unsigned)linearSpaceFree());
        PortDbgX("  app memory free", (unsigned)osGetMemRegionFree(MEMREGION_APPLICATION));
        PortDbgX("  game arena wanted (GameState_Realloc)", gPortDbgRealloc[0]);
        PortDbgX("  system heap largest free block", gPortDbgRealloc[1]);
    }
    Port3ds_LogFlush();
    if (sLogFile != NULL) {
        fflush(sLogFile);
    }
    svcBreak(USERBREAK_PANIC);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

/* hex value logger for diagnostics (e.g. scene-data dump) */
void PortDbgX(const char* label, unsigned val) {
    /* PORT (2026-09-21): format hex MANUALLY — sprintf("%s=%08x") is broken in the port's
     * libc (produces an empty buffer, which Log()'s empty-string guard then drops), so every
     * PortDbgX diagnostic was silently invisible. Manual formatting fixes all value-logging. */
    char buf[128]; int n = 0; int i;
    static const char hx[] = "0123456789abcdef";
    if (label) { while (label[n] != '\0' && n < 100) { buf[n] = label[n]; n++; } }
    buf[n++] = '='; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) buf[n++] = hx[(val >> i) & 0xF];
    buf[n] = '\0';
    Log(buf);
}

/* Fast file-only loggers for high-volume renderer tracing (no console print). */
void PortLogFast(const char* s) {
    if (s == NULL || s[0] == '\0') return;
    if (sLogAsync) { /* the writer thread owns the file now */
        Log_Append(s);
        return;
    }
    if (!sLogFile) sLogFile = fopen(LOG_PATH, "a");
    if (sLogFile) { fputs(s, sLogFile); fputc('\n', sLogFile); }
}
void PortLogFastX(const char* label, unsigned val) {
    /* hex by hand like PortDbgX: the port's sprintf("%s=%08x") gives an empty string, so these lines were dropped */
    char buf[96];
    int n = 0, i;
    static const char hx[] = "0123456789abcdef";
    if (label) { while (label[n] != '\0' && n < 80) { buf[n] = label[n]; n++; } }
    buf[n++] = '='; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) buf[n++] = hx[(val >> i) & 0xF];
    buf[n] = '\0';
    PortLogFast(buf);
}

/* Return the end address (base+size) of the mapped, readable memory block that
 * contains `addr`, or 0 if `addr` is unmapped/unreadable. The display-list
 * interpreter uses this to bound its reads: an un-terminated or garbage DL that
 * would otherwise walk into unmapped memory and data-abort is stopped cleanly at
 * the edge of its mapped block. One svcQueryMemory per 4KB page walked = cheap. */
/* PORT PERF (2026-09-30): the display-list walker and texture imports check every new memory block
 * with svcQueryMemory (a kernel call). The app's memory map is fixed after boot (heaps are reserved up
 * front), so readable ranges already confirmed are remembered: 8 most recent, round-robin. */
u32 gPortPerfMemQueries, gPortPerfMemHits;
u64 gPortPerfAudOpTicks[32]; /* audio_microcode.c: ticks per microcode op (perf_stages) */
unsigned PortMem_ReadableEnd(unsigned addr) {
    /* per thread (renderer, audio worker, game): the entries are written without a lock (2026-10-03) */
    static __thread unsigned sBase[8], sEnd[8];
    static __thread int sNext;
    MemInfo mi;
    PageInfo pi;
    int i;
    for (i = 0; i < 8; i++) {
        if (addr - sBase[i] < sEnd[i] - sBase[i]) {
            gPortPerfMemHits++;
            return sEnd[i];
        }
    }
    gPortPerfMemQueries++;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return 0;
    if (mi.state == MEMSTATE_FREE || mi.state == MEMSTATE_RESERVED) return 0;
    if (!(mi.perm & MEMPERM_READ)) return 0;
    sBase[sNext] = mi.base_addr;
    sEnd[sNext] = mi.base_addr + mi.size;
    sNext = (sNext + 1) & 7;
    return mi.base_addr + mi.size;
}

/* Keep the bottom-screen console up so the message is readable instead of
 * silently bouncing back to the HOME menu. */
static void boot_halt(const char* msg) {
    Log(msg);
    printf("Press START to exit.\n");
    boot_flush();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

/* ===== the ROM-free package (PORT 2026-11-07, user request: an installer others can use) =====
 * `make -f Makefile.3ds cia-romfree` removes every word of the game's data from the program (tools/port/
 * gen_asset_tables.py --romfree) and leaves a recipe; at boot PortRomFree_Restore puts the data back from the player's
 * own ROM, before anything uses it. In a normal build gPortRomFree keeps its placeholder and nothing happens.
 * Header (u32): magic "MMRF", version, the ROM size needed, the CRC-32 of every restored word in order, the recipe's
 * size, the largest range in bytes, the number of runs holding the recipe, 0, then (address, bytes) per run.
 * Recipe: range count; per range: destination, ROM offset, words, op stream bytes, the op stream (padded to 4).
 * Op stream tokens t = op << 5 | n: op 0..4 a run of n + 1 words (n == 31: a u32 count follows); op 7: n + 1 words,
 * one op per nibble (low first). Ops: 0 leave the word, 1 copy, 2 / 3 / 4 memory byte k = ROM byte k ^ 3 / 1 / 7. */
volatile u32 gPortRomFree[256] __attribute__((aligned(16))) = { 0xFFFFFFFFu };
#define ROMFREE_MAGIC 0x46524D4Du
#ifndef RF_PTR
#define RF_PTR(a) ((u8*)(uintptr_t)(a)) /* (a host test maps the 3DS addresses) */
#define RF_HDR gPortRomFree
#endif

static u32 sRfCrcTab[256];
static u32 RfCrc(u32 crc, const u8* p, u32 n) { /* zlib's crc32 */
    crc = ~crc;
    while (n--) crc = sRfCrcTab[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}
static void RfRun(u8* d, const u8* rom, u32 w, u32 n, u32 op, u32* crc) {
    static const u8 kX[5] = { 0, 0, 3, 1, 7 };
    u32 o = w * 4, end = (w + n) * 4, x = kX[op & 7], k;
    if (op == 0 || op > 4) return;
    if (x == 0) {
        memcpy(d + o, rom + o, end - o);
    } else {
        for (k = o; k < end; k++) d[k] = rom[k ^ x];
    }
    *crc = RfCrc(*crc, d + o, end - o);
}
/* 1 restored, 0 nothing to do (a normal build), < 0 failed: -1 not the right ROM, others no memory / no ROM file */
static int PortRomFree_Restore(const char* romPath) {
    volatile u32* h = RF_HDR;
    u8 *rec, *buf;
    u32 i, k, q, cnt, crc = 0, spans, recSize, maxBytes;
    long romSize;
    FILE* f;
    if (h[0] != ROMFREE_MAGIC) return 0;
    if (h[1] != 1) return -2;
    recSize = h[4], maxBytes = h[5], spans = h[6];
    rec = (u8*)malloc(recSize + 4);
    buf = (u8*)malloc(maxBytes + 16);
    if (rec == NULL || buf == NULL) return -3;
    for (i = 0, q = 0; i < spans && i < 124; i++) { /* the recipe out of the runs it is kept in (restored below) */
        memcpy(rec + q, RF_PTR(h[8 + 2 * i]), h[9 + 2 * i]);
        q += h[9 + 2 * i];
    }
    if ((f = fopen(romPath, "rb")) == NULL) return -4;
    fseek(f, 0, SEEK_END);
    romSize = ftell(f);
    if (romSize < 0 || (u32)romSize < h[2]) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < 256; i++) {
        u32 c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        sRfCrcTab[i] = c;
    }
    memcpy(&cnt, rec, 4);
    q = 4;
    for (i = 0; i < cnt; i++) {
        u32 hd[4], w = 0, n8, got;
        const u8* s;
        u8* d;
        memcpy(hd, rec + q, 16);
        q += 16;
        s = rec + q;
        q += (hd[3] + 3) & ~3u;
        n8 = (hd[2] * 4 + 7) & ~7u;
        fseek(f, (long)hd[1], SEEK_SET);
        got = (u32)fread(buf, 1, n8, f);
        if (got < n8) memset(buf + got, 0, n8 - got);
        d = RF_PTR(hd[0]);
        while (w < hd[2]) {
            u32 t = *s++, op = t >> 5, n = t & 31;
            if (op == 7) {
                for (k = 0; k <= n; k++) RfRun(d, buf, w + k, 1, (s[k >> 1] >> ((k & 1) * 4)) & 15, &crc);
                s += (n + 2) / 2;
                w += n + 1;
            } else {
                u32 len = n + 1;
                if (n == 31) {
                    memcpy(&len, s, 4);
                    s += 4;
                }
                RfRun(d, buf, w, len, op, &crc);
                w += len;
            }
        }
        Port3ds_CacheFlush(d, hd[2] * 4);
    }
    fclose(f);
    free(buf);
    free(rec);
    return crc == h[3] ? 1 : -1;
}

/* First launch of the shareable package: the player's ROM, as dumped (.z64, .n64 or .v64, any name, in sdmc:/3ds/mm/),
 * becomes baserom-decompressed.z64 the way tools/decompress_baserom.py makes it (every file at its VROM address, Yaz0
 * undone, dmadata rewritten, padded) - the port reads the ROM in that form. Majora's Mask (USA) only (NZSE). */
#ifndef ROM_DIR
#define ROM_DIR "sdmc:/3ds/mm/"
#endif
#define ROM_DMADATA 0x1A500
static int sRomOrder; /* 0 z64 (big-endian), 1 n64 (32-bit little-endian), 2 v64 (16-bit swapped) */
static int RomRead(FILE* f, u32 off, u8* dst, u32 n) { /* big-endian bytes [off, off + n) of the dump */
    u32 a = off & ~3u, end = (off + n + 3) & ~3u, i;
    u8* tmp = (u8*)malloc(end - a);
    if (tmp == NULL) return 0;
    fseek(f, (long)a, SEEK_SET);
    if (fread(tmp, 1, end - a, f) != end - a) {
        free(tmp);
        return 0;
    }
    if (sRomOrder == 1) {
        for (i = 0; i < end - a; i += 4) {
            u8 b0 = tmp[i], b1 = tmp[i + 1];
            tmp[i] = tmp[i + 3], tmp[i + 1] = tmp[i + 2], tmp[i + 2] = b1, tmp[i + 3] = b0;
        }
    } else if (sRomOrder == 2) {
        for (i = 0; i < end - a; i += 2) {
            u8 b = tmp[i];
            tmp[i] = tmp[i + 1], tmp[i + 1] = b;
        }
    }
    memcpy(dst, tmp + (off - a), n);
    free(tmp);
    return 1;
}
static u32 Be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static int Yaz0Dec(const u8* src, u32 srcLen, u8* dst, u32 dstLen) {
    u32 sp = 16, dp = 0;
    if (srcLen < 16 || memcmp(src, "Yaz0", 4) != 0) return 0;
    while (dp < dstLen) {
        u32 code, bit;
        if (sp >= srcLen) return 0;
        code = src[sp++];
        for (bit = 0; bit < 8 && dp < dstLen; bit++, code <<= 1) {
            if (code & 0x80) {
                if (sp >= srcLen) return 0;
                dst[dp++] = src[sp++];
            } else {
                u32 b1, b2, dist, n;
                if (sp + 1 >= srcLen) return 0;
                b1 = src[sp++], b2 = src[sp++];
                dist = (((b1 & 15) << 8) | b2) + 1;
                n = b1 >> 4;
                if (n == 0) {
                    if (sp >= srcLen) return 0;
                    n = src[sp++] + 0x12;
                } else {
                    n += 2;
                }
                if (dist > dp) return 0;
                while (n-- && dp < dstLen) {
                    dst[dp] = dst[dp - dist];
                    dp++;
                }
            }
        }
    }
    return 1;
}
static int RomWriteZeros(FILE* o, u32 n) {
    static const u8 z[4096];
    while (n) {
        u32 k = n > sizeof(z) ? (u32)sizeof(z) : n;
        if (fwrite(z, 1, k, o) != k) return 0;
        n -= k;
    }
    return 1;
}
/* 1 made, 0 no dump found, < 0 failed (-1: not Majora's Mask (USA)) */
static int PortRom_Prepare(const char* outPath) {
    char in[300] = "", tmpPath[300];
    u8 hdr[0x40], ent[16];
    u32 nent = 0, i, pos = 0, last = 0;
    u32 (*tab)[4];
    FILE *f, *o;
    DIR* dir = opendir(ROM_DIR);
    struct dirent* de;
    if (dir == NULL) return 0;
    while ((de = readdir(dir)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l > 4 && strcasecmp(de->d_name, "baserom-decompressed.z64") != 0 &&
            (strcasecmp(de->d_name + l - 4, ".z64") == 0 || strcasecmp(de->d_name + l - 4, ".n64") == 0 ||
             strcasecmp(de->d_name + l - 4, ".v64") == 0)) {
            snprintf(in, sizeof(in), "%s%s", ROM_DIR, de->d_name);
            break;
        }
    }
    closedir(dir);
    if (in[0] == '\0' || (f = fopen(in, "rb")) == NULL) return 0;
    {
        u8 m[4];
        if (fread(m, 1, 4, f) != 4) {
            fclose(f);
            return -1;
        }
        sRomOrder = (m[0] == 0x80 && m[1] == 0x37) ? 0 : (m[0] == 0x40 && m[1] == 0x12) ? 1 : (m[0] == 0x37) ? 2 : -1;
    }
    if (sRomOrder < 0 || !RomRead(f, 0, hdr, sizeof(hdr)) || memcmp(hdr + 0x3B, "NZSE", 4) != 0) {
        fclose(f);
        return -1;
    }
    Log("Preparing your ROM (first launch only, about a minute)...");
    boot_flush();
    tab = (u32(*)[4])malloc(4096 * sizeof(*tab));
    if (tab == NULL) {
        fclose(f);
        return -2;
    }
    for (nent = 0; nent < 4096; nent++) { /* dmadata: vrom start, end, rom start, end */
        if (!RomRead(f, ROM_DMADATA + nent * 16, ent, 16)) break;
        for (i = 0; i < 4; i++) tab[nent][i] = Be32(ent + i * 4);
        if (tab[nent][1] == 0) break;
    }
    snprintf(tmpPath, sizeof(tmpPath), "%s.part", outPath);
    if ((o = fopen(tmpPath, "wb")) == NULL) {
        free(tab);
        fclose(f);
        return -2;
    }
    for (i = 0; i < nent; i++) {
        u32 vs = tab[i][0], ve = tab[i][1], ps = tab[i][2], pe = tab[i][3], size = ve - vs;
        u8 *src = NULL, *dst;
        if (ps == 0xFFFFFFFFu && pe == 0xFFFFFFFFu) continue; /* (the syms entries: nothing in the ROM) */
        if (vs < pos || ve < vs) continue;
        if (!RomWriteZeros(o, vs - pos)) goto fail;
        pos = vs;
        dst = (u8*)malloc(size ? size : 1);
        if (dst == NULL) goto fail;
        if (pe != 0) {
            src = (u8*)malloc(pe - ps);
            if (src == NULL || !RomRead(f, ps, src, pe - ps) || !Yaz0Dec(src, pe - ps, dst, size)) {
                free(src);
                free(dst);
                goto fail;
            }
            free(src);
        } else if (!RomRead(f, ps, dst, size)) {
            free(dst);
            goto fail;
        }
        if (fwrite(dst, 1, size, o) != size) {
            free(dst);
            goto fail;
        }
        free(dst);
        pos = ve, last = ve;
        if ((i & 63) == 0) {
            char b[64];
            snprintf(b, sizeof(b), "  %u%%", (unsigned)(i * 100 / nent));
            Log(b);
            boot_flush();
        }
    }
    { /* padded to 128 KB, then dmadata as decompress_baserom.py writes it: (vrom start, end, vrom start, 0) */
        u32 padEnd = ((nent ? tab[nent - 1][1] : last) + 0x1FFFF) & ~0x1FFFFu;
        if (padEnd < pos) padEnd = pos;
        if (!RomWriteZeros(o, padEnd - pos)) goto fail;
        fseek(o, ROM_DMADATA, SEEK_SET);
        for (i = 0; i < nent; i++) {
            u32 e[4] = { tab[i][0], tab[i][1], tab[i][2], tab[i][3] }, k;
            if (!(e[2] == 0xFFFFFFFFu && e[3] == 0xFFFFFFFFu)) e[2] = e[0], e[3] = 0;
            for (k = 0; k < 4; k++) ent[k * 4] = e[k] >> 24, ent[k * 4 + 1] = e[k] >> 16, ent[k * 4 + 2] = e[k] >> 8,
                                    ent[k * 4 + 3] = e[k];
            if (fwrite(ent, 1, 16, o) != 16) goto fail;
        }
    }
    fclose(o);
    fclose(f);
    free(tab);
    remove(outPath);
    if (rename(tmpPath, outPath) != 0) return -2;
    Log("ROM ready.");
    return 1;
fail:
    fclose(o);
    fclose(f);
    free(tab);
    remove(tmpPath);
    return -2;
}

/* ===== MINIMAL BOOT TEST (disabled) =====
 * Isolation build used to prove the CIA packaging. Kept for future debugging.
 * Renamed out of the way; the real entry point is main() below. */
int main_minimal(int argc, char** argv) {
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    int n = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        printf("\x1b[2;2HOoT port MINIMAL boot test -- frame %d   ", n++);
        printf("\x1b[4;2HIf you can read this, packaging is OK.");
        printf("\x1b[6;2HPress START to exit.");
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}

/* Delete any existing Luma crash dumps at boot. Luma writes crash_dump_%08u.dmp
 * using the lowest free index, so with the folder emptied each launch, this
 * session's crash (if any) always lands as crash_dump_00000000.dmp — one file,
 * same name, which makes the user's Mac auto-transfer trivial. */
static void WipeCrashDumps(void) {
    const char* dir = "sdmc:/luma/dumps/arm11";
    DIR* d = opendir(dir);
    if (d == NULL) return;
    struct dirent* ent;
    char path[300];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
}

/* full boot path — the real entry point */
/* PORT DEBUG: svcOutputDebugString markers show up in Azahar's log with timestamps —
 * reliable boot-progress tracing before the SD boot.log is even open. */
#define DBG(s) svcOutputDebugString((s), sizeof(s) - 1)

/* PORT (2026-09-29): first thing in main, before any engine code: prove the process started and record
 * how it was launched and what memory it got (a hardware CIA launch showed nothing at all). */
static void Port_EarlyBootMarker(void) {
    FILE* f = fopen("sdmc:/3ds/mm/boot_early.log", "a"); /* own file: boot.log is recreated later */
    if (f != NULL) {
        bool isNew = false;
        u64 programId = 0;
        APT_CheckNew3DS(&isNew);
        APT_GetProgramID(&programId);
        fprintf(f, "=== main() reached: program %016llx, %s launch, %s 3DS, app mem %lu KB (free %lu KB), linear free %lu KB ===\n",
                (unsigned long long)programId, envIsHomebrew() ? "3dsx" : "CIA/3ds", isNew ? "New" : "Old",
                (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
                (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
                (unsigned long)(linearSpaceFree() / 1024));
        fclose(f);
    }
}

/* PORT (2026-10-04): the system re-applies the title's CPU settings when the game comes back from the HOME menu or
 * sleep: the system-core share granted to the audio mixer (Old 3DS, audio_3ds.c) and the New 3DS speed mode are lost
 * (found by the gdx-3ds port on hardware). Without the share the Old 3DS's mixer would stall the game after HOME. */
static aptHookCookie sCpuStateHook;
volatile int gPortAptResumed; /* set when the game comes back from HOME / sleep: the next update logs it */
static void Port3ds_CpuStateHook(APT_HookType type, void* param) {
    (void)param;
    { /* PORT (2026-11-08): diagnostic (a freeze on HOME, user report): each HOME / sleep step in boot.log, at once */
        extern void PortDbgX(const char* label, unsigned val);
        extern void Port3ds_LogFlush(void);
        PortDbgX("[apt] hook (0 suspend, 1 restore, 2 sleep, 3 wakeup, 4 exit)", (unsigned)type);
        Port3ds_LogFlush();
    }
    if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) gPortAptResumed = 1, sAptAway = 0, gPortAptSuspended = 0, gPortAptStep = 0;
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) sAptAway = 1;
    if (type == APTHOOK_ONSUSPEND) gPortAptStep = 1, gPortAptSuspended = 1; /* (the flip presenter stops presenting) */
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT) {
        Port3ds_BottomLight(1); /* (the cutscene light's screen off: not left off outside the game) */
    }
    if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) {
        extern unsigned gPortAudioCore1Limit;
        extern int gPortO3dsSim;
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (n3ds) {
            osSetSpeedupEnable(!gPortO3dsSim);
        }
        if (gPortAudioCore1Limit != 0) {
            APT_SetAppCpuTimeLimit(gPortAudioCore1Limit);
        }
        if (type == APTHOOK_ONWAKEUP || type == APTHOOK_ONRESTORE) {
            extern void Port3ds_AudioResume(void);
            Port3ds_AudioResume(); /* the DSP's queue is gone: no audio after sleep otherwise */
        }
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Port_EarlyBootMarker();
    { extern void PortOverlayStatics_Init(void); PortOverlayStatics_Init(); } /* before any game code */
    DBG("PORT: main() entered");
    {
        /* PORT (2026-10-26): the first start after installing a new build showed a black top screen until the game was
         * started again (user report, every build). The HOME menu starts a newly installed title the first time without
         * its exheader's New 3DS memory mode (SystemModeExt 124MB), and the renderer's buffers do not fit in the smaller
         * application region. In that case the game asks to be started again (chainload to itself) and quits: the second
         * start gets the right memory mode. A marker file stops it from looping if the memory never comes. */
        bool n3ds = false;
        u32 appMem = osGetMemRegionSize(MEMREGION_APPLICATION);
        APT_CheckNew3DS(&n3ds);
        if (n3ds && appMem < 0x7000000u) { /* < 112 MB: not the 124 MB mode */
            FILE* m = fopen("sdmc:/3ds/mm/relaunch.flag", "rb");
            if (m == NULL) {
                m = fopen("sdmc:/3ds/mm/relaunch.flag", "wb");
                if (m != NULL) {
                    fprintf(m, "app memory %u\n", (unsigned)appMem);
                    fclose(m);
                }
                aptSetChainloaderToSelf();
                return 0;
            }
            fclose(m); /* already tried once: go on with what there is */
        }
        remove("sdmc:/3ds/mm/relaunch.flag");
    }
    /* PORT PERF (2026-09-28): New 3DS: run the app core at 804 MHz with the L2 cache (default is the
     * original 268 MHz mode, ~3x slower). No effect on an original 3DS. */
    osSetSpeedupEnable(true);
    aptHook(&sCpuStateHook, Port3ds_CpuStateHook, NULL);
    gfxInitDefault();
    DBG("PORT: gfxInitDefault done");
    consoleInit(GFX_BOTTOM, NULL);
    DBG("PORT: consoleInit done");
    { extern void PortCompat_InitStreams(void); PortCompat_InitStreams(); }
    WipeCrashDumps(); /* keep only this run's crash dump, named crash_dump_00000000.dmp */

    /* Truncate the log file at the start of every boot (PORT 2026-10-04: the previous one is kept as boot_prev.log,
     * so a second session - or a restart after a crash - does not erase it). */
    {
        FILE* in = fopen(LOG_PATH, "rb");
        if (in != NULL) {
            FILE* out = fopen("sdmc:/3ds/mm/boot_prev.log", "wb");
            static char buf[16 * 1024];
            size_t n;
            while (out != NULL && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
                fwrite(buf, 1, n, out);
            }
            if (out != NULL) fclose(out);
            fclose(in);
        }
    }
    { FILE* f = fopen(LOG_PATH, "w");
      if (f) { fputs("=== MM 3DS boot log ===\n", f); fclose(f); } }

    { extern const char gPortVersion[]; /* Makefile.3ds: git describe, or port/VERSION */
      char line[160] = "The Legend of Zelda: Majora's Mask N64 3DS Port ";
      strncat(line, gPortVersion, 60);
      strcat(line, " booting...");
      Log(line); }
    /* ndsp plumbing AFTER the log is set up so its init status is visible (was before the
     * truncation above, which wiped its logs). Tries HLE even without dspfirm.cdc. */
    { extern void Port3ds_AudioInit(void); Port3ds_AudioInit(); }

    /* PORT DEBUG: the port assumes the linear heap is at 0x08000000 (segment-8 collision
     * handling). Log where libctru's linear heap actually lands on this Azahar/firmware. */
    { void* _lp = linearAlloc(0x1000);
      void* _lp2 = linearAlloc(0x100000);
      char _b[128];
      snprintf(_b, sizeof(_b), "MEM: linear base=%08x  +1MB=%08x  (24MB set)",
               (unsigned)(uintptr_t)_lp, (unsigned)(uintptr_t)_lp2);
      Log(_b);
      if (_lp) linearFree(_lp); if (_lp2) linearFree(_lp2); }

    FILE* rf = fopen(ROM_PATH, "rb");
    if (rf == NULL) {
        /* PORT (2026-11-07): the ROM as dumped, in sdmc:/3ds/mm/, is turned into the form the port reads (once) */
        int made = PortRom_Prepare(ROM_PATH);
        if (made <= 0) {
            boot_halt(made == -1 ? "This ROM is not The Legend of Zelda: Majora's Mask (USA) for the N64."
                      : made == 0 ? "No ROM found. Put your Majora's Mask (USA) N64 ROM (.z64, .n64 or .v64) in "
                                    "sdmc:/3ds/mm/ and start again."
                                  : "Could not prepare the ROM (is the SD card full?).");
            gfxExit();
            return 0;
        }
        rf = fopen(ROM_PATH, "rb");
    }
    if (rf == NULL) { boot_halt("ROM not found at " ROM_PATH); gfxExit(); return 0; }
    fclose(rf);
    Log("ROM found.");
    { /* PORT (2026-11-07): the shareable package carries none of the game's data: put it back from this ROM */
        int r = PortRomFree_Restore(ROM_PATH);
        if (r < 0) {
            boot_halt(r == -1 ? "This ROM does not match: the port needs The Legend of Zelda: Majora's Mask (USA) "
                                "for the N64, unmodified."
                              : "Could not load the game data from the ROM (out of memory?).");
            gfxExit();
            return 0;
        }
        if (r > 0) Log("Game data loaded from your ROM.");
    }

    PortDma_Init(ROM_PATH);
    { /* PORT (2026-11-12): the sound data, read into memory just now, must be the US ROM's (a damaged copy on a 2DS's SD
       * card stopped the game at its first soundfont, with no hint why). 0x67D55A6F: CRC-32 of 0x20700..0x5E06E0. */
        extern int PortDma_AudioCheck(u32* crc);
        u32 crc = 0;
        if (PortDma_AudioCheck(&crc) && crc != 0x67D55A6Fu) {
            PortDbgX("ROM sound data CRC (want 67d55a6f)", (unsigned)crc);
            boot_halt("Your ROM file is damaged: sdmc:/3ds/mm/baserom-decompressed.z64 doesn't match the US ROM. "
                      "Copy it to the SD card again (or delete it and put your original ROM dump in sdmc:/3ds/mm/).");
            gfxExit();
            return 0;
        }
    }
    {
        FILE* fb;
        hidScanInput();
        if ((hidKeysHeld() & KEY_L) && (fb = fopen("sdmc:/3ds/mm/settings_b.txt", "r")) != NULL) {
            fclose(fb);
            sSettingsPath = "sdmc:/3ds/mm/settings_b.txt";
            Log("settings: settings_b.txt (L held at start)");
        }
    }
    Port3ds_LoadSettings();
    Port3ds_ProfStart();
    {
        extern int gPortO3dsSim;
        if (gPortO3dsSim) {
            osSetSpeedupEnable(false);
            DBG("PORT: o3ds_sim: New 3DS speedup OFF (268 MHz, no L2) - Old 3DS approximation");
        }
    }

    Log("DMA init OK.");
    { /* PORT MM (bring-up): physical addresses of the main regions, to place GPU address errors */
      extern char port_seg_gameplay_keep[], __end__[]; extern int gLoBuffer;
      void* h = malloc(16); void* l = linearAlloc(16);
      PortDbgX("phys .data asset (gameplay_keep)", (unsigned)osConvertVirtToPhys(port_seg_gameplay_keep));
      PortDbgX("  virt", (unsigned)(uintptr_t)port_seg_gameplay_keep);
      PortDbgX("phys .bss (gLoBuffer)", (unsigned)osConvertVirtToPhys(&gLoBuffer));
      PortDbgX("phys image end", (unsigned)osConvertVirtToPhys(__end__));
      PortDbgX("phys app heap", (unsigned)osConvertVirtToPhys(h)); PortDbgX("  virt", (unsigned)(uintptr_t)h);
      PortDbgX("phys linear", (unsigned)osConvertVirtToPhys(l)); PortDbgX("  virt", (unsigned)(uintptr_t)l);
      { extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
        PortDbgX("linear heap virt", (unsigned)__ctru_linear_heap); PortDbgX("linear heap size", (unsigned)__ctru_linear_heap_size);
        PortDbgX("linear space free", (unsigned)linearSpaceFree()); }
      free(h); linearFree(l); }
    /* PORT (2026-09-24): bootproc() normally calls Locale_Init (cart header -> gCurrentRegion,
     * which SaveContext_Init turns into the save language). The port enters Main() directly,
     * so region stayed 0 and the US ROM showed Japanese text. Run it here, after the ROM opens. */
    /* PORT MM TODO: OoT ran Locale_Init() here for the save language; check MM's region/language setup */

    gViConfigModeType = 0;

#ifdef PORT_LOG_SYNC
    PortDbgX("fb top", (unsigned)(uintptr_t)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL));
    PortDbgX("fb bottom", (unsigned)(uintptr_t)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
#endif
    Log("calling Main() (engine init)...");
    Main(0);
    Log("Main() returned; entering graph loop.");
    Port3ds_FreezeWatchStart(); /* (a freeze from here on is logged with where it is) */
#ifdef PORT_LOG_SYNC
    PortDbgX("after Main: fb top", (unsigned)(uintptr_t)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL));
    PortDbgX("after Main: fb bottom", (unsigned)(uintptr_t)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    PortDbgX("after Main: linear free", (unsigned)linearSpaceFree());
#endif

    {
        extern int Port3ds_AudioReady(void);
        extern void PortAudio_Init(void);
        extern void AudioSfx_Reset(void);
        if (Port3ds_AudioReady()) {
            PortAudio_Init();
            gPortMmAudio = 1;
            Log("[audio] MM audio engine initialized");
        } else {
            AudioSfx_Reset();
            Log("[audio] MM audio disabled: DSP output unavailable");
        }
    }

    Port3ds_TouchUiInit(); /* boot finished: the bottom screen becomes the control panel */
#ifndef PORT_LOG_SYNC /* (debug: PORT_EXTRA=-DPORT_LOG_SYNC keeps every line written at once, for crash hunting) */
    Log_StartAsync();      /* from here on the SD card is written by a background thread */
#endif
    {
        /* PORT PERF (2026-10-04): the Old 3DS layout. Its second core (1) is shared with the system, which gives an
         * application only the share granted by APT_SetAppCpuTimeLimit. Measured on hardware at Old 3DS speed: the game
         * loop there (v50) ran 41 instead of 60 updates per second with crackling audio; the drawing there (v51) took
         * 72-128 ms per frame instead of 25-40 on core 0, and the game, waiting for each frame, fell to 7-13 updates
         * per second. (Both ran with 30% of the core, not the 55% asked for: the exheader's MaxCpu refused it. v54 raises
         * the limit to 89% and asks for 80% - worth measuring again.) So an Old 3DS draws on core 0 with the game (no render thread), and only the audio mixer (~22% of a
         * core) uses core 1. o3ds_layout=1 reproduces this on a New 3DS (mixer on core 1, no render thread) for faithful
         * Old 3DS tests with o3ds_sim=1. */
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (!n3ds || sO3dsLayout) {
            extern int gPortAudioCore;
            gPortAudioCore = 1;
            sRenderThreadSetting = sRenderThreadAB = 0;
            Log("[render] Old 3DS layout: one core for the game and the drawing, audio mixer on core 1");
        }
    }
    Graph_ThreadEntry(0);

    { extern void PortFlashrom_WaitIdle(void); PortFlashrom_WaitIdle(); }
    boot_halt("graph loop exited");
    gfxExit();
    return 0;
}
