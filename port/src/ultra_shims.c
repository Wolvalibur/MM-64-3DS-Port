#include <string.h>
/*
 * ultra_shims.c — libultra replacement layer for the OoT port.
 * Single-threaded synchronous model: the platform frame loop owns execution;
 * N64 threads are recorded but never scheduled. Message queues are real.
 */
#include "ultra64.h"
/* freestanding bring-up: libc via implicit decls (32-bit: int==ptr) */
extern int fprintf();
extern void* stderr;
struct PortTimespec { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct PortTimespec* ts);
#define PORT_CLOCK_MONOTONIC 1
extern void* memset();

/* ------------------------------------------------------------------ */
/* Message queues — faithful ring-buffer semantics, minus blocking.    */
/* In a single-threaded world a blocking recv on an empty queue would  */
/* deadlock; we log it so bring-up shows us each spot that needs the   */
/* frame loop to feed it. */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    if (mq == NULL) { return -1; }
    if (mq->validCount >= mq->msgCount) {
        (void)flag;
        return -1; /* BLOCK defused: single-threaded bring-up */
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    if (mq == NULL) { return -1; }
    if (mq->validCount >= mq->msgCount) {
        (void)flag;
        return -1; /* BLOCK defused: single-threaded bring-up */
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    if (mq == NULL) { return -1; }
    if (mq->validCount == 0) {
        (void)flag;
        return -1; /* BLOCK defused: single-threaded bring-up */
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Threads — recorded, never run. The frame loop calls what matters.   */

static OSThread* sThreads[32];
static int sThreadCount = 0;

void osCreateThread(OSThread* t, OSId id, void* entry, void* arg, void* sp, OSPri pri) { /* (MM prototype) */
    (void)sp;
    t->id = id;
    t->priority = pri;
    if (sThreadCount < 32) {
        sThreads[sThreadCount++] = t;
    }
    fprintf(stderr, "[shim] thread %d created (entry %p) — not scheduled\n", (int)id, (void*)entry);
}

void osStartThread(OSThread* t) { (void)t; }
void osStopThread(OSThread* t) { (void)t; }
void osDestroyThread(OSThread* t) { (void)t; }
void osYieldThread(void) {}
OSId osGetThreadId(OSThread* t) { return (t != NULL) ? t->id : 0; }
OSPri osGetThreadPri(OSThread* t) { return (t != NULL) ? t->priority : 0; }
void osSetThreadPri(OSThread* t, OSPri pri) { if (t != NULL) { t->priority = pri; } }

/* ------------------------------------------------------------------ */
/* Events, interrupts, timers                                          */

static OSMesgQueue* sEventQueues[OS_NUM_EVENTS];
static OSMesg sEventMesgs[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg) {
    if (e < OS_NUM_EVENTS) {
        sEventQueues[e] = mq;
        sEventMesgs[e] = msg;
    }
}

/* The platform loop calls this to deliver VI retrace etc. */
void PortShim_RaiseEvent(OSEvent e) {
    if (e < OS_NUM_EVENTS && sEventQueues[e] != NULL) {
        osSendMesg(sEventQueues[e], sEventMesgs[e], OS_MESG_NOBLOCK);
    }
}

OSIntMask osSetIntMask(OSIntMask mask) { (void)mask; return OS_IM_ALL; }
u32 osGetIntMask(void) { return OS_IM_ALL; }

#ifdef __3DS__
extern u64 svcGetSystemTick(void);
/* OS_CLOCK_RATE ticks (46.875 MHz counter on N64) from the 268.111856 MHz system tick: tick * 46875000 / 268111856
 * as a 32.32 fixed-point product (clock_gettime's 64-bit software divisions were measurable on the Old 3DS) */
static u64 TicksNow(void) {
    const u64 k = 750905219ULL; /* 46875000 / 268111856 * 2^32 */
    u64 t = svcGetSystemTick();
    return (t >> 32) * k + (((t & 0xFFFFFFFFULL) * k) >> 32);
}
#else
static u64 TicksNow(void) {
    struct PortTimespec ts;
    clock_gettime(PORT_CLOCK_MONOTONIC, &ts);
    /* OS_CLOCK_RATE ticks (46.875 MHz counter on N64) */
    return (u64)ts.tv_sec * 46875000ULL + (u64)ts.tv_nsec * 46875ULL / 1000000ULL;
}
#endif

OSTime osGetTime(void) { return TicksNow(); }
void osSetTime(OSTime t) { (void)t; }
u32 osGetCount(void) { return (u32)TicksNow(); }

int osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    (void)timer; (void)countdown; (void)interval; (void)mq; (void)msg;
    return 0; /* frame loop will deliver time-based messages */
}
int osStopTimer(OSTimer* timer) { (void)timer; return 0; }

/* ------------------------------------------------------------------ */
/* Cache / TLB / misc CPU — meaningless on the host                    */

void osInvalDCache(void* addr, size_t size) { (void)addr; (void)size; }
void osInvalICache(void* addr, size_t size) { (void)addr; (void)size; }
void osWritebackDCache(void* addr, s32 size) { (void)addr; (void)size; }
void osWritebackDCacheAll(void) {}
u32 osVirtualToPhysical(void* addr) { return (u32)(uintptr_t)addr; }
void osMapTLBRdb(void) {}
void osUnmapTLBAll(void) {}
s32 osAfterPreNMI(void) { return 0; }
void __osInitialize_common(void) {} /* (MM: osInitialize() is a macro for it) */
u32 osGetMemSize(void) { return 8 * 1024 * 1024; }

/* ------------------------------------------------------------------ */
/* VI — frame presentation is the platform layer's job                 */

static void* sNextFb = NULL;
static void* sCurrentFb = NULL;

void osViSetMode(OSViMode* mode) { (void)mode; }
void osViSetSpecialFeatures(u32 func) { (void)func; }
void osViSetXScale(f32 v) { (void)v; }
void osViSetYScale(f32 v) { (void)v; }
void osViExtendVStart(u32 v) { (void)v; }
void osViBlack(u8 active) { (void)active; }
static u32 sSwapCount = 0;
void osViSwapBuffer(void* fb) {
    sNextFb = fb;
    sSwapCount++;
    if ((sSwapCount % 60) == 1) {
        fprintf(stderr, "[vi] frame %d (fb %p)\n", sSwapCount, fb);
    }
}
void* osViGetNextFramebuffer(void) { return sNextFb; }
void* osViGetCurrentFramebuffer(void) { return (sCurrentFb != NULL) ? sCurrentFb : sNextFb; }
void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    (void)retraceCount;
    osSetEventMesg(OS_EVENT_VI, mq, msg);
}
void osCreateViManager(OSPri pri) { (void)pri; }

/* ------------------------------------------------------------------ */
/* Controller — one native controller on port 1                       */

s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    (void)mq;
    *bitpattern = 1; /* controller 1 present */
    osContGetQuery(status);
    return 0;
}
s32 osContStartReadData(OSMesgQueue* mq) { PortShim_RaiseEvent(OS_EVENT_SI); (void)mq; return 0; }

/* keyboard -> pad, implemented in pc_gfx.c */
extern unsigned short PortInput_GetPad(signed char* outX, signed char* outY);

void osContGetReadData(OSContPad* pad) {
    signed char sx = 0, sy = 0;
    s32 i;
    memset(pad, 0, 4 * sizeof(OSContPad));
    pad[0].button = PortInput_GetPad(&sx, &sy);
    pad[0].stick_x = sx;
    pad[0].stick_y = sy;
    for (i = 1; i < MAXCONTROLLERS; i++) {
        pad[i].errno = CHNL_ERR_NORESP >> 4;
    }
}
s32 osContStartQuery(OSMesgQueue* mq) { (void)mq; return 0; }
void osContGetQuery(OSContStatus* status) {
    s32 i;
    memset(status, 0, 4 * sizeof(OSContStatus));
    status[0].type = CONT_TYPE_NORMAL;
    for (i = 1; i < MAXCONTROLLERS; i++) {
        status[i].errno = CHNL_ERR_NORESP >> 4;
    }
}
s32 osContSetCh(u8 ch) { (void)ch; return 0; }

/* Pump the game's PadMgr from live 3DS buttons once per frame. Normally a
 * dedicated controller thread runs PadMgr_HandleRetrace on each VI retrace, but
 * in the synchronous model that thread never executes, so gPadMgr.inputs[] stays
 * zero and the game ignores input. MM also requires UpdateConnections before
 * UpdateInputs: an unregistered controller's readings are discarded.
 * Called from PortGfx_RunTask (3ds_main.c) after the hardware poll. */
#include "padmgr.h"
extern void PadMgr_UpdateConnections(void); /* global in padmgr.c, not in the header */
extern void PadMgr_UpdateInputs(void);
void Port3ds_PumpInput(void) {
    osContGetReadData(gPadMgr.pads); /* pads[0] <- live 3DS buttons via the shim above */
    osContGetQuery(gPadMgr.padStatus);
    PadMgr_UpdateConnections();
    PadMgr_UpdateInputs(); /* fills inputs[] incl. press/release edges */
}

/* Pump the audio driver once per frame. AudioMgr_ThreadEntry never runs
 * (osStartThread is a no-op), so AudioThread_Update is never called and no audio
 * RSP task is ever built or dispatched. Drive the per-retrace handler directly:
 * it forwards the previous frame's Acmd task to the scheduler (-> the C audio
 * microcode in audio_microcode.c) and builds the next one. Mirrors the PadMgr
 * pattern above. Called from PortGfx_RunTask (3ds_main.c). */
#include "audiomgr.h"
#include "regs.h"
#include "global.h" /* PORT: was OoT audio.h */
extern AudioMgr sAudioMgr;                         /* global instance in main.c */
static void Port3ds_PumpAudio_impl(void);
void Port3ds_PumpAudio(void) {
    extern int gPortMmAudio;
    if (!gPortMmAudio) {
        return;
    }
    {
    extern volatile unsigned char gPortProf;
    extern void Port3ds_ScanBetweenUpdates(void);
    unsigned char prev = gPortProf;
    Port3ds_ScanBetweenUpdates(); /* (3ds_main.c: taps between updates) */
    gPortProf = 10; /* PROF_AUDIO (port_prof.h) */
    Port3ds_PumpAudio_impl();
    gPortProf = prev;
    }
}
void PortAudio_Init(void) {
    Audio_Init();
    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    Audio_InitSound();
    osSendMesg(&sAudioMgr.lockQueue, NULL, OS_MESG_NOBLOCK);
}

static void Port3ds_PumpAudio_impl(void) {
    extern void Port3ds_AudioTaskRun(void* task);
    extern void Port3ds_AudioTaskWait(void);
    extern u64 gPortPerfAudioMain;
    extern u64 svcGetSystemTick(void);
    AudioTask* previous = R_AUDIOMGR_DEBUG_LEVEL > 0 ? NULL : sAudioMgr.rspTask;
    AudioTask* next;
    u64 start = svcGetSystemTick();

    if (previous != NULL) {
        Port3ds_AudioTaskRun(&previous->task);
    }
    next = R_AUDIOMGR_DEBUG_LEVEL >= 2 ? NULL : AudioThread_Update();
    if (previous != NULL) {
        Port3ds_AudioTaskWait();
        if (previous->taskQueue != NULL) {
            osSendMesg(previous->taskQueue, NULL, OS_MESG_NOBLOCK);
        }
    }
    sAudioMgr.rspTask = next;
    gPortPerfAudioMain += svcGetSystemTick() - start;
}

/* Rumble + Controller Pak: absent hardware */
s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, s32 channel) { (void)mq; (void)pfs; (void)channel; return 1; }
s32 __osMotorAccess(OSPfs* pfs, s32 flag) { (void)pfs; (void)flag; return 1; }

/* PORT (2026-09-29): touch panel state (3ds_main.c draws it; see port_minimap.h and
 * docs/3ds-touch-panel.md). The game side fills gPortMinimap / gPortHud*; the input side sets the
 * gPortTouch* one-shot requests that z_player.c consumes. */
#include "port_minimap.h"
#include "segment_symbols.h"
#include "sys_cmpdma.h"
PortMinimap gPortMinimap;
int gPortMinimapOnBottom = 1;
const unsigned char* gPortHudIconSeg;
unsigned int gPortHudSerial;
int gPortHudKeys = -1;
int gPortHudTatl;
int gPortHudTop = 1;
volatile int gPortTouchOcarina;
volatile int gPortTouchPage = -1;
static PortHudInfo sHudInfo;
int gPortMaskSlotItem = 0xFF; /* ITEM_NONE */
/* PORT (2026-11-08): the ZR mask of each save file (settings.txt mask_slot<file>), so it is there again after a restart.
 * It was one value for all files, and the title screen's demo (another inventory) emptied it before a file was loaded
 * (user report: "I have to re-equip the mask slot every time I boot up"). */
int gPortMaskSlotFile[PORT_MASK_SLOT_FILES] = { 0xFF, 0xFF, 0xFF, 0xFF };
volatile unsigned gPortMaskSlotSerial; /* bumped when one changes: 3ds_main.c saves settings.txt */
volatile int gPortMaskAssignReq;
volatile int gPortInPause;
static u8 sHudIcons[4 * ICON_ITEM_TEX_SIZE] __attribute__((aligned(8)));
static const u8* sHudIconSource;

/* PORT (2026-11-06): the MM3D-style touch panel (3ds_main.c): an item or mask dragged from its Items / Masks page onto
 * X, Y or I is equipped there, as on the pause screen (KaleidoScope_UpdateItemEquip: an item already on another C
 * button swaps with what the target held; the bow and its magic arrows share one button), and a mask dropped on II
 * goes on the ZR mask slot. Done here, on the game thread, between updates. */
volatile int gPortEquipReq, gPortMaskSlotReq;
extern u8 gPlayerFormSlotRestrictions[PLAYER_FORM_MAX][ITEM_NUM_SLOTS]; /* z_kaleido_item.c */
static const u16 sPortMoonMaskBits[24] = { /* z_kaleido_mask.c sMasksGivenOnMoonBits: (byte << 8) | bit */
    0x101, 0x004, 0x202, 0x180, 0x104, 0x210, 0x010, 0x201, 0x008, 0x110, 0x204, 0x220,
    0x040, 0x080, 0x002, 0x102, 0x001, 0x240, 0x120, 0x108, 0x020, 0x140, 0x208, 0x280,
};
static int PortMaskGivenOnMoon(int maskIndex) {
    return (gSaveContext.masksGivenOnMoon[sPortMoonMaskBits[maskIndex] >> 8] & (u8)sPortMoonMaskBits[maskIndex]) != 0;
}
static int PortIsBowItem(int item) {
    return item == ITEM_BOW || (item >= ITEM_BOW_FIRE && item <= ITEM_BOW_LIGHT);
}
static void PortTouch_Equip(PlayState* play, int target, int slot) {
    static const u8 sFormItems[PLAYER_FORM_MAX] = { ITEM_MASK_FIERCE_DEITY, ITEM_MASK_GORON, ITEM_MASK_ZORA, ITEM_MASK_DEKU,
                                                    ITEM_NONE };
    Player* player = GET_PLAYER(play);
    int item, cur, o;
    if (target < EQUIP_SLOT_C_LEFT || target > EQUIP_SLOT_C_RIGHT || slot < 0 || slot >= 2 * ITEM_NUM_SLOTS ||
        player == NULL || IS_PAUSED(&play->pauseCtx) || gSaveContext.gameMode != GAMEMODE_NORMAL ||
        Play_InCsMode(play) || play->msgCtx.msgMode != MSGMODE_NONE || play->transitionMode != TRANS_MODE_OFF) {
        Audio_PlaySfx(NA_SE_SY_ERROR);
        return;
    }
    item = gSaveContext.save.saveInfo.inventory.items[slot];
    if (item == ITEM_NONE || (slot >= ITEM_NUM_SLOTS && PortMaskGivenOnMoon(slot - ITEM_NUM_SLOTS))) {
        Audio_PlaySfx(NA_SE_SY_ERROR);
        return;
    }
    cur = BUTTON_ITEM_EQUIP(0, target);
    /* (the pause screen's checks) the mask Link wears, or the transformation he is in, stays on its button; an item
     * in his hand stays on the button it came from; no Deku / Goron mask under water */
    if ((cur != ITEM_NONE && cur == sFormItems[GET_PLAYER_FORM]) ||
        (Player_GetCurMaskItemId(play) != ITEM_NONE && Player_GetCurMaskItemId(play) == cur) ||
        (player->heldItemAction != PLAYER_IA_NONE && player->heldItemButton == target) ||
        ((item == ITEM_MASK_DEKU || item == ITEM_MASK_GORON) &&
         Player_GetEnvironmentalHazard(play) >= PLAYER_ENV_HAZARD_UNDERWATER_FLOOR &&
         Player_GetEnvironmentalHazard(play) <= PLAYER_ENV_HAZARD_UNDERWATER_FREE)) {
        Audio_PlaySfx(NA_SE_SY_ERROR);
        return;
    }
    if (item >= ITEM_ARROW_FIRE && item <= ITEM_ARROW_LIGHT) { /* a magic arrow: the bow with it, on the bow's slot */
        if (INV_CONTENT(ITEM_BOW) != ITEM_BOW) {
            Audio_PlaySfx(NA_SE_SY_ERROR);
            return;
        }
        item = ITEM_BOW_FIRE + (item - ITEM_ARROW_FIRE);
        slot = SLOT_BOW;
    }
    for (o = EQUIP_SLOT_C_LEFT; o <= EQUIP_SLOT_C_RIGHT; o++) {
        if (o == target) {
            continue;
        }
        /* the item (or, for the bow, a magic arrow form of it) is already on another C button: it takes what the
         * target held */
        if (C_SLOT_EQUIP(0, o) == slot || (item == ITEM_BOW && PortIsBowItem(BUTTON_ITEM_EQUIP(0, o)))) {
            if (cur != ITEM_NONE) {
                BUTTON_ITEM_EQUIP(0, o) = cur;
                C_SLOT_EQUIP(0, o) = C_SLOT_EQUIP(0, target);
                Interface_LoadItemIcon(play, o);
            } else {
                BUTTON_ITEM_EQUIP(0, o) = ITEM_NONE;
                C_SLOT_EQUIP(0, o) = SLOT_NONE;
            }
        }
    }
    BUTTON_ITEM_EQUIP(0, target) = item;
    C_SLOT_EQUIP(0, target) = slot;
    Interface_LoadItemIconImpl(play, target);
    Audio_PlaySfx(NA_SE_SY_DECIDE);
}

/* the ZR mask slot (port_minimap.h), every game update - also while paused, when Interface_Draw (PortHud_Update) does
 * not run: that is where ZR assigns, so assigning never happened (user report: "ZR still opens the menu") */
void PortMaskSlot_Update(PlayState* play) {
    static s32 sActive = false, sFile = -1;
    Player* player = GET_PLAYER(play);
    s32 file = gSaveContext.fileNum;

    gPortInPause = IS_PAUSED(&play->pauseCtx);
    /* not in the title screen's demo, nor in any mode that isn't a loaded file */
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        sActive = false;
        return;
    }
    if ((file < 0) || (file >= PORT_MASK_SLOT_FILES)) {
        file = -1;
    }
    if (!sActive || (file != sFile)) { /* a file was loaded: its mask goes back on ZR */
        sActive = true;
        sFile = file;
        gPortMaskSlotItem = (file >= 0) ? gPortMaskSlotFile[file] : ITEM_NONE;
    }
    if (gPortEquipReq) { /* the touch panel: an item dropped on X, Y or I */
        int req = gPortEquipReq;
        gPortEquipReq = 0;
        PortTouch_Equip(play, req >> 8, (req & 0xFF) - 1);
    }
    if (gPortMaskSlotReq) { /* the touch panel: a mask dropped on II */
        int item = gPortMaskSlotReq - 1;
        gPortMaskSlotReq = 0;
        if (item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT && INV_CONTENT(item) == item &&
            !PortMaskGivenOnMoon(SLOT(item) - ITEM_NUM_SLOTS)) {
            gPortMaskSlotItem = item;
            Audio_PlaySfx(NA_SE_SY_DECIDE);
        } else {
            Audio_PlaySfx(NA_SE_SY_ERROR);
        }
    }
    if (gPortMaskAssignReq) { /* ZR on the Masks page: the mask under the cursor goes on ZR */
        gPortMaskAssignReq = 0;
        if (gPortInPause && play->pauseCtx.pageIndex == PAUSE_MASK) {
            int item = play->pauseCtx.cursorItem[PAUSE_MASK];
            if (item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT && INV_CONTENT(item) == item) {
                gPortMaskSlotItem = item;
                Audio_PlaySfx(NA_SE_SY_DECIDE);
            } else {
                Audio_PlaySfx(NA_SE_SY_ERROR);
            }
        }
    }
    if (gPortMaskSlotItem != ITEM_NONE && INV_CONTENT(gPortMaskSlotItem) != gPortMaskSlotItem) {
        gPortMaskSlotItem = ITEM_NONE; /* (another save file, or not owned) */
    }
    /* an empty slot takes the first mask Link puts on, so ZR works without a trip to the Masks page */
    if (gPortMaskSlotItem == ITEM_NONE && player != NULL && player->currentMask != PLAYER_MASK_NONE) {
        int item = Player_MaskIdToItemId(player->currentMask - 1);
        if (item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT && INV_CONTENT(item) == item) {
            gPortMaskSlotItem = item;
        }
    }
    if ((sFile >= 0) && (gPortMaskSlotFile[sFile] != gPortMaskSlotItem)) { /* kept for this file */
        gPortMaskSlotFile[sFile] = gPortMaskSlotItem;
        gPortMaskSlotSerial++;
    }
}

/* an ammo count at the most it can hold: the HUD draws it green then (z_parameter.c Interface_DrawAmmoCount) */
static int PortAmmoFull(int item, int ammo) {
    if (item >= ITEM_BOW_FIRE && item <= ITEM_BOW_LIGHT) item = ITEM_BOW;
    switch (item) {
        case ITEM_BOW: return ammo == CUR_CAPACITY(UPG_QUIVER);
        case ITEM_BOMB:
        case ITEM_BOMBCHU: return ammo == CUR_CAPACITY(UPG_BOMB_BAG);
        case ITEM_DEKU_STICK: return ammo == CUR_CAPACITY(UPG_DEKU_STICKS);
        case ITEM_DEKU_NUT: return ammo == CUR_CAPACITY(UPG_DEKU_NUTS);
        case ITEM_POWDER_KEG:
        case ITEM_PICTOGRAPH_BOX: return ammo == 1;
        case ITEM_MAGIC_BEANS: return ammo == 20;
    }
    return 0;
}

void PortHud_Update(PlayState* play) {
    PortHudInfo* h = &sHudInfo;
    SavePlayerData* data = &gSaveContext.save.saveInfo.playerData;
    Player* player = GET_PLAYER(play);
    u8 previousItems[3];
    int i;
    memcpy(previousItems, h->cItem, sizeof(previousItems));
    memset(h, 0, sizeof(*h));
    h->valid = gSaveContext.gameMode == GAMEMODE_NORMAL;
    h->cutscene = Play_InCsMode(play) ? 1 : 0;
    h->health = data->health;
    {
        int k;
        h->lifeSize = (unsigned char)play->interfaceCtx.lifeSizeChange;
        for (k = 0; k < 3; k++) {
            h->beatPrim[k] = (unsigned char)play->interfaceCtx.beatingHeartPrim[k];
            h->beatEnv[k] = (unsigned char)play->interfaceCtx.beatingHeartEnv[k];
        }
    }
    h->healthCapacity = data->healthCapacity;
    h->magic = data->magic;
    h->magicCapacity = data->isMagicAcquired ? (data->isDoubleMagicAcquired ? 96 : 48) : 0;
    h->rupees = data->rupees;
    h->wallet = (unsigned char)(CUR_UPG_VALUE(UPG_WALLET) > 2 ? 2 : CUR_UPG_VALUE(UPG_WALLET));
    h->rupeesFull = data->rupees == CUR_CAPACITY(UPG_WALLET);
    h->defHearts = (unsigned char)CLAMP(gSaveContext.save.saveInfo.inventory.defenseHearts, 0, 20);
    h->keys = -1;
    switch (play->sceneId) {
        case SCENE_INISIE_N:
        case SCENE_INISIE_R:
        case SCENE_MITURIN:
        case SCENE_HAKUGIN:
        case SCENE_SEA:
            h->keys = DUNGEON_KEY_COUNT(gSaveContext.mapIndex);
            break;
    }
    h->ocarina = INV_CONTENT(ITEM_OCARINA_OF_TIME);
    h->mask = gPortMaskSlotItem; /* the MASKS pad shows the ZR mask (PortMaskSlot_Update) */
    h->notebook = CHECK_QUEST_ITEM(QUEST_BOMBERS_NOTEBOOK) != 0;
    for (i = 0; i < 2 * ITEM_NUM_SLOTS; i++) { /* the touch panel's Items and Masks pages */
        int item = gSaveContext.save.saveInfo.inventory.items[i];
        if (i >= ITEM_NUM_SLOTS && item != ITEM_NONE && PortMaskGivenOnMoon(i - ITEM_NUM_SLOTS)) {
            item = ITEM_NONE;
        }
        h->inv[i] = item;
        if (i < ITEM_NUM_SLOTS) {
            h->invAmmo[i] = (item != ITEM_NONE && gAmmoItems[i] != ITEM_NONE) ? gSaveContext.save.saveInfo.inventory.ammo[i]
                                                                              : -1;
            h->invAmmoFull[i] = h->invAmmo[i] >= 0 && PortAmmoFull(item, h->invAmmo[i]);
            h->invDim[i] = !gPlayerFormSlotRestrictions[GET_PLAYER_FORM][i];
        }
    }
    for (i = 0; i < 4; i++) {
        h->remains |= (CHECK_QUEST_ITEM(QUEST_REMAINS_ODOLWA + i) != 0) << i;
    }
    {
        static const u8 sSongs[10] = { QUEST_SONG_SONATA, QUEST_SONG_LULLABY, QUEST_SONG_BOSSA_NOVA, QUEST_SONG_ELEGY,
                                       QUEST_SONG_OATH,   QUEST_SONG_TIME,    QUEST_SONG_HEALING,    QUEST_SONG_EPONA,
                                       QUEST_SONG_SOARING, QUEST_SONG_STORMS };
        for (i = 0; i < 10; i++) {
            h->songs += CHECK_QUEST_ITEM(sSongs[i]) != 0;
        }
    }
    { /* the name, from the file select's characters: 0-9, A-Z, a-z, then space - . */
        int n = 0;
        for (i = 0; i < 8; i++) {
            u8 c = data->playerName[i];
            char ch = c <= 9 ? '0' + c : (c >= 0x0A && c <= 0x23) ? 'A' + (c - 0x0A)
                    : (c >= 0x24 && c <= 0x3D) ? 'a' + (c - 0x24) : c == 0x3F ? '-' : c == 0x40 ? '.' : ' ';
            h->name[i] = ch;
            if (ch != ' ') {
                n = i + 1;
            }
        }
        h->name[n] = '\0';
    }
    h->day = gSaveContext.save.day;
    h->minutes = ((u32)CURRENT_TIME * 1440) >> 16;
    for (i = 0; i < 3; i++) {
        int item = GET_CUR_FORM_BTN_ITEM(i + EQUIP_SLOT_C_LEFT);
        h->cItem[i] = item;
        if (item < ITEM_F0 && (item != previousItems[i] || sHudIconSource != play->interfaceCtx.iconItemSegment)) {
            memcpy(sHudIcons + (i + 1) * ICON_ITEM_TEX_SIZE,
                   play->interfaceCtx.iconItemSegment + (i + 1) * ICON_ITEM_TEX_SIZE, ICON_ITEM_TEX_SIZE);
        }
        h->cDisabled[i] = gSaveContext.buttonStatus[i + EQUIP_SLOT_C_LEFT] == BTN_DISABLED;
        h->cSlot[i] = C_SLOT_EQUIP(0, i + EQUIP_SLOT_C_LEFT);
        h->cAmmo[i] = -1;
        if (item >= ITEM_BOW_FIRE && item <= ITEM_BOW_LIGHT) {
            item = ITEM_BOW;
        }
        switch (item) {
            case ITEM_DEKU_STICK:
            case ITEM_DEKU_NUT:
            case ITEM_BOMB:
            case ITEM_BOW:
            case ITEM_BOMBCHU:
            case ITEM_POWDER_KEG:
            case ITEM_MAGIC_BEANS:
                h->cAmmo[i] = AMMO(item);
                break;
            case ITEM_PICTOGRAPH_BOX:
                h->cAmmo[i] = CHECK_QUEST_ITEM(QUEST_PICTOGRAPH) != 0;
                break;
        }
        h->cAmmoFull[i] = h->cAmmo[i] >= 0 && PortAmmoFull(item, h->cAmmo[i]);
    }
    sHudIconSource = play->interfaceCtx.iconItemSegment;
    gPortHudIconSeg = sHudIcons;
    gPortHudKeys = h->keys;
    gPortHudTatl = play->interfaceCtx.tatlCalling;
    gPortHudSerial++;
    gPortTouchPage = -1;
    {
        extern void PortDebug_Chests(PlayState* play);
        PortDebug_Chests(play);
    }
}

/* PORT (2026-10-26): chest diagnostics (user report: the Deku Nuts chest at the start would not open, A did nothing).
 * The code path (z_en_box.c EnBox_WaitOpen -> Actor_OfferGetItemNearby -> z_player.c chest action on A) reads fine, so
 * this logs what each check sees while Link stands within 70 units of a chest, to sdmc:/3ds/mm/boot.log: a line set
 * when something changes (at most every 5 frames) and at every A press. Cheap: only chests near Link are looked at. */
#include "overlays/actors/ovl_En_Box/z_en_box.h"
void PortDebug_Chests(PlayState* play) {
    extern void PortDbgX(const char* label, unsigned val);
    static u32 sLastKey;
    static s32 sLastFrame;
    Player* player = GET_PLAYER(play);
    Actor* actor;
    s32 pressA, frame = play->state.frames;

    if (player == NULL) {
        return;
    }
    pressA = CHECK_BTN_ALL(play->state.input[0].press.button, BTN_A) != 0;
    for (actor = play->actorCtx.actorLists[ACTORCAT_CHEST].first; actor != NULL; actor = actor->next) {
        EnBox* box = (EnBox*)actor;
        Vec3f offset;
        s16 facing;
        u32 key;

        if (actor->id != ACTOR_EN_BOX || actor->xzDistToPlayer > 70.0f) {
            continue;
        }
        Actor_WorldToActorCoords(actor, &offset, &player->actor.world.pos);
        facing = (s16)(BINANG_ADD(actor->yawTowardsPlayer, 0x8000) - player->actor.shape.rot.y);
        key = ((u32)((s32)offset.x >> 2) & 0xFF) | (((u32)((s32)offset.z >> 2) & 0xFF) << 8) |
              (((u32)((s32)offset.y >> 1) & 0xFF) << 16) | ((u32)((facing >> 11) & 0x1F) << 24) |
              ((u32)(player->getItemId != 0) << 29) | ((u32)(player->interactRangeActor == actor) << 30) |
              ((u32)(player->stateFlags1 >> 29) << 31);
        if (!pressA && (key == sLastKey || (frame - sLastFrame) < 5)) {
            continue;
        }
        sLastKey = key;
        sLastFrame = frame;
        PortDbgX(pressA ? "[chest] A pressed near chest, frame" : "[chest] near chest, frame", (u32)frame);
        PortDbgX("[chest]   offset x (link rel. chest, need -20<x<20)", (u32)(s32)offset.x);
        PortDbgX("[chest]   offset y (need -10<y<10)", (u32)(s32)offset.y);
        PortDbgX("[chest]   offset z (need -50<z<0)", (u32)(s32)offset.z);
        PortDbgX("[chest]   facing diff (need |d|<0x3000)", (u32)(s32)facing);
        PortDbgX("[chest]   xzDist / heightRel (x100)",
                 ((u32)(s32)(actor->xzDistToPlayer * 100.0f) << 16) | ((u32)(s32)(actor->playerHeightRel * 100.0f) & 0xFFFF));
        PortDbgX("[chest]   box getItemId", (u32)box->getItemId);
        PortDbgX("[chest]   box type/moveFlags/unk_1EC",
                 ((u32)box->type << 24) | ((u32)box->movementFlags << 16) | ((u32)(u16)box->unk_1EC));
        PortDbgX("[chest]   box opened flag", (u32)Flags_GetTreasure(play, ENBOX_GET_CHEST_FLAG(actor)));
        PortDbgX("[chest]   player getItemId", (u32)(s32)player->getItemId);
        PortDbgX("[chest]   player interactRangeActor is this chest", (u32)(player->interactRangeActor == actor));
        PortDbgX("[chest]   player stateFlags1", player->stateFlags1);
        PortDbgX("[chest]   player stateFlags2", player->stateFlags2);
        PortDbgX("[chest]   player csAction/form", ((u32)player->csAction << 8) | player->transformation);
        PortDbgX("[chest]   cutscene now", (u32)(s32)CutsceneManager_GetCurrentCsId());
    }
}

void Port_GetHudInfo(PortHudInfo* h) {
    *h = sHudInfo;
    if (gPortHudSerial == 0) {
        memset(h, 0, sizeof(*h));
        h->rupees = h->keys = -1;
        h->ocarina = ITEM_NONE;
        h->mask = ITEM_NONE;
        memset(h->inv, ITEM_NONE, sizeof(h->inv));
        memset(h->cSlot, SLOT_NONE, sizeof(h->cSlot));
    }
}

const unsigned char* Port_GetItemIcon(int itemId) {
    static u64 icons[3][ICON_ITEM_TEX_SIZE / sizeof(u64)];
    static int ids[3] = { -1, -1, -1 };
    /* (0x61: the archive's Bomber's Notebook icon, gItemIconBombersNotebookTex - a quest item, not an item id) */
    int slot = itemId == ITEM_OCARINA_OF_TIME ? 0 : (itemId == 0x61 ? 2 : 1);
    if (itemId < 0 || (itemId > ITEM_MASK_GIANT && itemId != 0x61)) {
        return NULL;
    }
    if (ids[slot] != itemId) {
        CmpDma_LoadFile(SEGMENT_ROM_START(icon_item_static_yar), itemId, icons[slot], ICON_ITEM_TEX_SIZE);
        ids[slot] = itemId;
    }
    return (const unsigned char*)icons[slot];
}

/* PORT (2026-11-06): the touch panel's item and mask grids and the Quest page's remains: up to 52 icons at once, kept
 * once loaded (Port_GetItemIcon keeps 3). Filled again from the start when full. */
#define PORT_ICON_CACHE 56
const unsigned char* Port_GetItemIconCached(int itemId) {
    static u64 sIcons[PORT_ICON_CACHE][ICON_ITEM_TEX_SIZE / sizeof(u64)];
    static s8 sIndex[ITEM_REMAINS_TWINMOLD + 1];
    static int sUsed, sInit;
    int k;
    if (itemId < 0 || itemId > ITEM_REMAINS_TWINMOLD || (itemId > ITEM_MASK_GIANT && itemId < ITEM_REMAINS_ODOLWA)) {
        return NULL;
    }
    if (!sInit) {
        memset(sIndex, -1, sizeof(sIndex));
        sInit = 1;
    }
    if (sIndex[itemId] < 0) {
        if (sUsed == PORT_ICON_CACHE) {
            memset(sIndex, -1, sizeof(sIndex));
            sUsed = 0;
        }
        k = sUsed++;
        CmpDma_LoadFile(SEGMENT_ROM_START(icon_item_static_yar), itemId, sIcons[k], ICON_ITEM_TEX_SIZE);
        sIndex[itemId] = (s8)k;
    }
    return (const unsigned char*)sIcons[sIndex[itemId]];
}

void PortMap_Begin(int sceneId) {
    gPortMinimap.serial++;
    gPortMinimap.sceneId = sceneId;
    gPortMinimap.numLayers = gPortMinimap.numMarks = 0;
    gPortMinimap.compass = gPortMinimap.hasStart = 0;
}

void PortMap_AddMark(int type, int x, int y, int r, int g, int b) {
    int i = gPortMinimap.numMarks;
    if (i == PORT_MINIMAP_MAX_MARKS) {
        return;
    }
    gPortMinimap.markType[i] = type;
    gPortMinimap.markX[i] = x;
    gPortMinimap.markY[i] = y;
    gPortMinimap.markR[i] = r;
    gPortMinimap.markG[i] = g;
    gPortMinimap.markB[i] = b;
    gPortMinimap.numMarks++;
}

/* PORT (2026-10-26): quality-of-life options from 2Ship2Harkinian (mm/2s2h/Enhancements), game side. The switches
 * (port_minimap.h) are set by the touch panel's SETUP page (3ds_main.c) and saved in settings.txt.
 *
 *   - skip the intro and the Song of Time "Dawn of..." transition (Cutscenes/SkipIntroSequence,
 *     Songs/SkipSoTCutscenes); the Double/Inverted Song of Time and Soaring cutscenes are skipped in their actors
 *   - Song of Double Time: choose the hour (Songs/BetterSongOfDoubleTime)
 *   - persistent owl saves and saving from the pause screen (Saving/SavingEnhancements, PauseSave)
 *   - the Bomber's Notebook from the touch panel
 */
#include "global.h"
#include "z64horse.h"
#include "assets/objects/gameplay_keep/gameplay_keep.h"
#include "segment_symbols.h"
#include "z64font.h"

int gPortFastText = 0;      /* PORT (2026-11-12): fast text and skip cutscenes start off on a first run (saved once set) */
int gPortSkipCutscenes = 0;
int gPortInstantPutaway = 1;
int gPortArrowCycle = 1;
int gPortOwlPersist = 1;
int gPortDoubleTime = 1;
int gPortPauseSave = 0;
int gPortGuardSight = 0;
int gPortCheats[CHEAT_TOGGLES]; /* all off */
int gPortEnh[ENH_COUNT]; /* all off */
const int gPortEnhCount = ENH_COUNT; /* 3ds_main.c's settings.txt reader */
const int gPortCheatCount = CHEAT_TOGGLES; /* 3ds_main.c's settings.txt reader */
volatile int gPortCheatGive;

volatile int gPortPauseSaveReq;    /* the touch panel's SAVE button (3ds_main.c) */
volatile int gPortPauseSaveResult; /* 1 saved, -1 not possible here: the panel shows it */
volatile int gPortNotebookReq;     /* the touch panel's NOTEBOOK button */
volatile int gPortCanPauseSave;    /* the SAVE button is usable now (paused, somewhere saving is allowed) */

/* 3ds_main.c: the entrance a pause save loads at, kept next to settings.txt (the save format is the N64's) */
extern void Port3ds_PauseSaveWrite(int fileNum, unsigned short entrance, unsigned short checksum);
extern int Port3ds_PauseSaveRead(int fileNum, unsigned short* entrance, unsigned short* checksum);
extern s32 PortSram_PauseOwlSave(PlayState* play);

/* ---------------- intro / Song of Time transition (Cutscene_HandleConditionalTriggers) ---------------- */

void PortEnh_TransitionCs(PlayState* play) {
    /* PORT (2026-11-08): cheat, always win minigames - the Goron race is won on the way in, and the baby Goron's
     * brother is waiting with the prize (2S2H DifficultyOptions/GoronRace "skip") */
    if (PORT_CHEAT_ON(CHEAT_MINIGAMES) && (gSaveContext.save.entrance == ENTRANCE(GORON_RACETRACK, 1))) {
        SET_EVENTINF(EVENTINF_11);
        gSaveContext.save.entrance = ENTRANCE(GORON_RACETRACK, 2);
    }
    if (!gPortSkipCutscenes) {
        return;
    }
    /* a new file's opening (Lost Woods, Skull Kid, the Clock Tower's inside): straight to South Clock Town as Deku Link,
     * with Tatl and the Deku Nuts from the chest, as 2S2H does */
    if ((gSaveContext.save.entrance == ENTRANCE(CUTSCENE, 0)) && (gSaveContext.save.cutsceneIndex == 0) &&
        !gSaveContext.save.isFirstCycle) {
        gSaveContext.save.entrance = ENTRANCE(SOUTH_CLOCK_TOWN, 0);
        gSaveContext.save.hasTatl = true;
        gSaveContext.cycleSceneFlags[SCENE_INSIDETOWER].switch0 |= (1 << 0);
        gSaveContext.cycleSceneFlags[SCENE_OPENINGDAN].switch0 |= (1 << 2);
        gSaveContext.cycleSceneFlags[SCENE_OPENINGDAN].switch0 |= (1 << 0);
        gSaveContext.save.playerForm = PLAYER_FORM_DEKU;
        gSaveContext.save.isFirstCycle = true;
        gSaveContext.cycleSceneFlags[SCENE_OPENINGDAN].chest |= (1 << 0);
        Item_Give(play, ITEM_DEKU_NUTS_10);
        Sram_SaveSpecialEnterClockTown(play);
        return;
    }
    /* Song of Time: the "Dawn of the First Day" cutscene entrance goes straight to its destination */
    if (((gSaveContext.save.entrance == ENTRANCE(CUTSCENE, 0)) || (gSaveContext.save.entrance == ENTRANCE(CUTSCENE, 1))) &&
        (gSaveContext.save.cutsceneIndex == 0xFFF7)) {
        gSaveContext.save.cutsceneIndex = 0;
        gSaveContext.save.eventDayCount = 0;
        gSaveContext.save.day = 0;
        gSaveContext.save.time = CLOCK_TIME(6, 0) - 1;
        if (gSaveContext.save.entrance == ENTRANCE(CUTSCENE, 1)) {
            gSaveContext.save.entrance = ENTRANCE(LOST_WOODS, 1); /* the first reset's flashback montage */
        } else {
            gSaveContext.save.entrance = ENTRANCE(SOUTH_CLOCK_TOWN, 0);
        }
    }
}

/* ---------------- Song of Double Time: choose the hour ---------------- */

#define CLOCK_TIME_NORMALIZED(time) ((u16)((time)-CLOCK_TIME(6, 0)))
#define CLOCK_TIME_MINUTE_F CLOCK_TIME_F(0, 1)

static s32 sDtActive;   /* the player is choosing the hour */
static s32 sDtPending;  /* chosen: applied when the scene reloads */
static u16 sDtOrigTime, sDtSelTime;
static s32 sDtOrigDay, sDtSelDay;
static s32 sDtShownDay = -1; /* the day texture loaded in the clock (doActionSegment) */
static s8 sDtRepeatDir, sDtRepeatTimer;
static f32 sDtArrowTween;
static s16 sDtArrowUp;

int PortDoubleTime_Active(void) {
    return sDtActive;
}

int PortDoubleTime_Pending(void) {
    return sDtPending;
}

static void PortDoubleTime_LoadDay(PlayState* play, s32 day) {
    s32 i = day - 1;

    if ((i < 0) || (i >= 3)) {
        i = 0;
    }
    if (sDtShownDay == i) {
        return;
    }
    sDtShownDay = i;
    DmaMgr_RequestSync(play->interfaceCtx.doActionSegment + DO_ACTION_OFFSET_DAY_NUMBER,
                       SEGMENT_ROM_START(week_static) + i * WEEK_STATIC_TEX_SIZE, WEEK_STATIC_TEX_SIZE);
}

/* z_message.c, the Song of Double Time was played: returns true when the hour picker took over */
int PortDoubleTime_Begin(PlayState* play) {
    if (!gPortDoubleTime) {
        return false;
    }
    if (gSaveContext.save.day >= 4) {
        Message_StartTextbox(play, 0x1B94, NULL);
        play->msgCtx.ocarinaMode = OCARINA_MODE_END;
        return true;
    }
    if (gSaveContext.save.day <= 0) {
        Message_StartTextbox(play, 0x1B95, NULL);
        play->msgCtx.ocarinaMode = OCARINA_MODE_PROCESS_RESTRICTED_SONG;
        return true;
    }
    Interface_SetAButtonDoAction(play, DO_ACTION_DECIDE);
    Interface_SetHudVisibility(HUD_VISIBILITY_A_B);
    play->msgCtx.ocarinaMode = OCARINA_MODE_PROCESS_DOUBLE_TIME;
    sDtActive = true;
    sDtPending = false;
    sDtOrigTime = sDtSelTime = CURRENT_TIME;
    sDtOrigDay = sDtSelDay = gSaveContext.save.day;
    sDtShownDay = sDtOrigDay - 1;
    sDtRepeatDir = 0;
    return true;
}

static void PortDoubleTime_Update(PlayState* play) {
    Input* input = CONTROLLER1(&play->state);
    s32 dir = 0;
    f32 interval = CLOCK_TIME_MINUTE_F * 30;

    if (!sDtActive) {
        return;
    }
    if (play->msgCtx.ocarinaMode != OCARINA_MODE_PROCESS_DOUBLE_TIME) { /* interrupted */
        sDtActive = false;
        PortDoubleTime_LoadDay(play, CURRENT_DAY);
        return;
    }
    play->interfaceCtx.bAlpha = 255;

    sDtArrowTween += sDtArrowUp ? -0.05f : 0.05f;
    if (sDtArrowTween >= 1.0f) {
        sDtArrowTween = 1.0f, sDtArrowUp = 1;
    } else if (sDtArrowTween <= 0.0f) {
        sDtArrowTween = 0.0f, sDtArrowUp = 0;
    }

    if (CHECK_BTN_ALL(input->press.button, BTN_B)) { /* cancel */
        Audio_PlaySfx_MessageCancel();
        play->msgCtx.ocarinaMode = OCARINA_MODE_END;
        sDtActive = false;
        PortDoubleTime_LoadDay(play, CURRENT_DAY);
        return;
    }
    if (CHECK_BTN_ALL(input->press.button, BTN_A) && (play->msgCtx.msgMode == MSGMODE_NONE)) { /* confirm */
        Audio_PlaySfx_MessageDecide();
        if ((sDtSelDay == sDtOrigDay) && (sDtSelTime == sDtOrigTime)) {
            play->msgCtx.ocarinaMode = OCARINA_MODE_END;
            sDtActive = false;
            return;
        }
        play->msgCtx.ocarinaMode = OCARINA_MODE_APPLY_DOUBLE_SOT;
        gSaveContext.timerStates[TIMER_ID_MOON_CRASH] = TIMER_STATE_OFF;
        sDtActive = false;
        sDtPending = true;
        return;
    }

    /* D-pad left/right: 30 minutes, repeating; the stick: up to 30 minutes by how far it is pushed */
    if (CHECK_BTN_ALL(input->cur.button, BTN_DLEFT) || CHECK_BTN_ALL(input->cur.button, BTN_DRIGHT)) {
        s32 d = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT) ? -1 : 1;

        if (sDtRepeatDir == d) {
            if (--sDtRepeatTimer < 0) {
                sDtRepeatTimer = 2;
                dir = d;
            }
        } else {
            sDtRepeatTimer = 10;
            sDtRepeatDir = d;
            dir = d;
        }
    } else {
        sDtRepeatDir = 0;
    }
    if (input->rel.stick_x < -5) {
        dir = -1;
        interval = CLOCK_TIME_MINUTE_F * CLAMP_MAX(input->rel.stick_x / -2, 30);
    } else if (input->rel.stick_x > 5) {
        dir = 1;
        interval = CLOCK_TIME_MINUTE_F * CLAMP_MAX(input->rel.stick_x / 2, 30);
    }
    if (CHECK_BTN_ALL(input->cur.button, BTN_Z)) {
        interval /= 6; /* Z (L): fine steps */
    } else if (CHECK_BTN_ALL(input->cur.button, BTN_R)) {
        interval *= 2; /* R: faster */
    }

    if (dir > 0) {
        u16 newTime = sDtSelTime + (u16)interval;

        if ((sDtSelDay == 3) && (sDtSelTime < CLOCK_TIME(6, 0)) && (newTime > CLOCK_TIME(6, 0) - CLOCK_TIME_HOUR)) {
            newTime = CLOCK_TIME(6, 0) - CLOCK_TIME_HOUR; /* no further than 5:00 on the final day */
            if ((sDtOrigDay == 3) && (CLOCK_TIME_NORMALIZED(newTime) < CLOCK_TIME_NORMALIZED(sDtOrigTime))) {
                newTime = sDtOrigTime;
            }
        }
        if ((newTime > CLOCK_TIME(6, 0)) && (sDtSelTime < CLOCK_TIME(6, 0))) {
            sDtSelDay = CLAMP(sDtSelDay + 1, sDtOrigDay, 3);
        }
        sDtSelTime = newTime;
    } else if (dir < 0) {
        u16 newTime = sDtSelTime - (u16)interval;

        if ((sDtSelDay == sDtOrigDay) && ((CLOCK_TIME_NORMALIZED(newTime) < CLOCK_TIME_NORMALIZED(sDtOrigTime)) ||
                                          ((u16)interval > CLOCK_TIME_NORMALIZED(sDtSelTime)))) {
            newTime = sDtOrigTime; /* not before now */
        }
        if ((newTime < CLOCK_TIME(6, 0)) && (sDtSelTime > CLOCK_TIME(6, 0))) {
            sDtSelDay = CLAMP(sDtSelDay - 1, sDtOrigDay, 3);
        }
        sDtSelTime = newTime;
    }
    PortDoubleTime_LoadDay(play, sDtSelDay);
}

/* z_parameter.c, around Interface_DrawClock: the clock shows the chosen hour while choosing */
static u16 sDtSaveTime;
static s32 sDtSaveDay;
void PortDoubleTime_ClockBegin(PlayState* play) {
    if (sDtActive) {
        sDtSaveTime = gSaveContext.save.time;
        sDtSaveDay = gSaveContext.save.day;
        gSaveContext.save.time = sDtSelTime;
        gSaveContext.save.day = sDtSelDay;
    }
}

void PortDoubleTime_ClockEnd(PlayState* play) {
    if (sDtActive) {
        gSaveContext.save.time = sDtSaveTime;
        gSaveContext.save.day = sDtSaveDay;

        /* left / right arrows beside the clock (2S2H draws them the same way) */
        OPEN_DISPS(play->state.gfxCtx);
        Gfx_SetupDL39_Overlay(play->state.gfxCtx);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, (u8)(255 - 155.0f * sDtArrowTween), (u8)(255 - 155.0f * sDtArrowTween),
                        (u8)(100.0f * sDtArrowTween), (u8)(200 - 50.0f * sDtArrowTween));
        gDPLoadTextureBlock(OVERLAY_DISP++, gArrowCursorTex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 24, 0,
                            G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, G_TX_NOMASK, G_TX_NOLOD,
                            G_TX_NOLOD);
        gSPTextureRectangle(OVERLAY_DISP++, 26 << 2, 191 << 2, (26 + 16) << 2, (191 + 24) << 2, G_TX_RENDERTILE,
                            0, 0, -1 << 10, 1 << 10);
        gSPTextureRectangle(OVERLAY_DISP++, 278 << 2, 191 << 2, (278 + 16) << 2, (191 + 24) << 2, G_TX_RENDERTILE,
                            0, 0, 1 << 10, 1 << 10);
        CLOSE_DISPS(play->state.gfxCtx);
    }
}

/* EnTest6_Destroy: the Double Song of Time cutscene is over - reload the scene, the chosen hour applies on the way
 * (PortDoubleTime_PlayDestroy), so nothing runs with the new time on the fly */
void PortDoubleTime_CutsceneDone(PlayState* play) {
    Player* player = GET_PLAYER(play);

    if (!sDtPending || (play->transitionTrigger != TRANS_TRIGGER_OFF) || (player == NULL)) {
        return;
    }
    play->nextEntrance = gSaveContext.save.entrance;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
    Play_SetRespawnData(play, RESPAWN_MODE_RETURN, gSaveContext.save.entrance, play->roomCtx.curRoom.num,
                        PLAYER_PARAMS(0xFF, PLAYER_START_MODE_B), &player->actor.world.pos, player->actor.world.rot.y);
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
    gSaveContext.respawnFlag = 2;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED; /* the new day's music starts */
}

void PortDoubleTime_PlayDestroy(void) {
    if (sDtPending) {
        sDtPending = false;
        gSaveContext.save.day = sDtSelDay;
        gSaveContext.save.time = sDtSelTime;
    }
    sDtActive = false;
    sDtShownDay = -1;
}

/* ---------------- saving from the pause screen ---------------- */

s32 PortEnh_CanSave(PlayState* play) {
    Player* player = GET_PLAYER(play);

    if ((player == NULL) || !gSaveContext.flashSaveAvailable || (gSaveContext.fileNum == 0xFF)) {
        return false;
    }
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }
    if (Player_InBlockingCsMode(play, player) || (play->msgCtx.msgMode != MSGMODE_NONE)) {
        return false;
    }
    switch (play->sceneId) { /* before Clock Town; the moon */
        case SCENE_SPOT00:
        case SCENE_LOST_WOODS:
        case SCENE_OPENINGDAN:
        case SCENE_SOUGEN:
        case SCENE_LAST_LINK:
        case SCENE_LAST_DEKU:
        case SCENE_LAST_GORON:
        case SCENE_LAST_ZORA:
        case SCENE_LAST_BS:
            return false;
        default:
            break;
    }
    if (CHECK_WEEKEVENTREG(WEEKEVENTREG_08_01) || CHECK_WEEKEVENTREG(WEEKEVENTREG_PLAYING_SWORDSMAN_MINIGAME) ||
        CHECK_WEEKEVENTREG(WEEKEVENTREG_PLAYING_FISHERMAN_JUMPING_GAME) || CHECK_WEEKEVENTREG(WEEKEVENTREG_KICKOUT_WAIT) ||
        CHECK_EVENTINF(EVENTINF_34) || CHECK_EVENTINF(EVENTINF_41)) {
        return false; /* minigames with temporary flags */
    }
    if ((gSaveContext.save.day < 1) || (gSaveContext.save.day > 3)) {
        return false;
    }
    return true;
}

/* where a pause save puts Link back: the entrance he came in by (temples: their entrance; grottos, cutscene
 * entrances: South Clock Town) */
static u16 PortEnh_PauseSaveEntrance(PlayState* play) {
    switch (play->sceneId) {
        case SCENE_MITURIN:
        case SCENE_MITURIN_BS:
            return ENTRANCE(WOODFALL_TEMPLE, 0);
        case SCENE_HAKUGIN:
        case SCENE_HAKUGIN_BS:
            return ENTRANCE(SNOWHEAD_TEMPLE, 0);
        case SCENE_SEA:
        case SCENE_SEA_BS:
            return ENTRANCE(GREAT_BAY_TEMPLE, 0);
        case SCENE_INISIE_N:
            return ENTRANCE(STONE_TOWER_TEMPLE, 0);
        case SCENE_INISIE_R:
        case SCENE_INISIE_BS:
            return ENTRANCE(STONE_TOWER_TEMPLE_INVERTED, 0);
        case SCENE_KAKUSIANA:
            return ENTRANCE(SOUTH_CLOCK_TOWN, 0);
        default:
            break;
    }
    if ((gSaveContext.save.entrance >> 9) == ENTR_SCENE_CUTSCENE) {
        return ENTRANCE(SOUTH_CLOCK_TOWN, 0);
    }
    return gSaveContext.save.entrance;
}

static void PortEnh_PauseSave(PlayState* play) {
    if (!PortEnh_CanSave(play) || !PortSram_PauseOwlSave(play)) {
        Audio_PlaySfx(NA_SE_SY_ERROR);
        gPortPauseSaveResult = -1;
        return;
    }
    Port3ds_PauseSaveWrite(gSaveContext.fileNum, PortEnh_PauseSaveEntrance(play), gSaveContext.save.saveInfo.checksum);
    Audio_PlaySfx(NA_SE_SY_PIECE_OF_HEART);
    gPortPauseSaveResult = 1;
}

/* z_sram_NES.c, loading an owl save: a pause save's own entrance replaces the owl statue's. Returns true for a pause
 * save (kept on load like a persistent owl save) */
int PortEnh_PauseSaveLoadEntrance(void) {
    unsigned short entrance, checksum;

    if (!Port3ds_PauseSaveRead(gSaveContext.fileNum, &entrance, &checksum)) {
        return false;
    }
    if (checksum != gSaveContext.save.saveInfo.checksum) {
        return false; /* saved at an owl statue since */
    }
    gSaveContext.save.entrance = entrance;
    return true;
}


/* ---------------- cheats (PORT 2026-10-28; port_minimap.h) ---------------- */

static void PortCheat_FillAmmo(void) {
    if (INV_CONTENT(ITEM_BOW) == ITEM_BOW) AMMO(ITEM_BOW) = CUR_CAPACITY(UPG_QUIVER);
    if (INV_CONTENT(ITEM_BOMB) == ITEM_BOMB) AMMO(ITEM_BOMB) = CUR_CAPACITY(UPG_BOMB_BAG);
    if (INV_CONTENT(ITEM_BOMBCHU) == ITEM_BOMBCHU) AMMO(ITEM_BOMBCHU) = CUR_CAPACITY(UPG_BOMB_BAG);
    if (INV_CONTENT(ITEM_DEKU_STICK) == ITEM_DEKU_STICK) AMMO(ITEM_DEKU_STICK) = CUR_CAPACITY(UPG_DEKU_STICKS);
    if (INV_CONTENT(ITEM_DEKU_NUT) == ITEM_DEKU_NUT) AMMO(ITEM_DEKU_NUT) = CUR_CAPACITY(UPG_DEKU_NUTS);
    if (INV_CONTENT(ITEM_MAGIC_BEANS) == ITEM_MAGIC_BEANS) AMMO(ITEM_MAGIC_BEANS) = 20;
    if (INV_CONTENT(ITEM_POWDER_KEG) == ITEM_POWDER_KEG) AMMO(ITEM_POWDER_KEG) = 1;
}

static void PortCheat_Upgrade(s16 upg, u32 value) {
    if (GET_CUR_UPG_VALUE(upg) < value) Inventory_ChangeUpgrade(upg, value);
}

static void PortCheat_GiveMagic(void) {
    if (!gSaveContext.save.saveInfo.playerData.isMagicAcquired) {
        gSaveContext.save.saveInfo.playerData.isMagicAcquired = true;
        gSaveContext.magicFillTarget = MAGIC_NORMAL_METER;
    }
}

static void PortCheat_Give(PlayState* play, int what) {
    static const u8 sItems[] = {
        ITEM_OCARINA_OF_TIME, ITEM_BOW,        ITEM_ARROW_FIRE,    ITEM_ARROW_ICE,       ITEM_ARROW_LIGHT,
        ITEM_BOMB,            ITEM_BOMBCHU,    ITEM_DEKU_STICK,    ITEM_DEKU_NUT,        ITEM_MAGIC_BEANS,
        ITEM_POWDER_KEG,      ITEM_PICTOGRAPH_BOX, ITEM_LENS_OF_TRUTH, ITEM_HOOKSHOT, ITEM_SWORD_GREAT_FAIRY,
    };
    static const u8 sSongs[] = {
        QUEST_SONG_SONATA, QUEST_SONG_LULLABY, QUEST_SONG_BOSSA_NOVA, QUEST_SONG_ELEGY,   QUEST_SONG_OATH,
        QUEST_SONG_TIME,   QUEST_SONG_HEALING, QUEST_SONG_EPONA,     QUEST_SONG_SOARING, QUEST_SONG_STORMS,
    };
    u32 i;

    switch (what) {
        case CHEAT_GIVE_ITEMS:
            /* the containers first, so the ammo items have somewhere to go */
            PortCheat_Upgrade(UPG_QUIVER, 1);
            PortCheat_Upgrade(UPG_BOMB_BAG, 1);
            PortCheat_Upgrade(UPG_DEKU_STICKS, 1);
            PortCheat_Upgrade(UPG_DEKU_NUTS, 1);
            PortCheat_GiveMagic(); /* the elemental arrows and the lens need it */
            for (i = 0; i < ARRAY_COUNT(sItems); i++) INV_CONTENT(sItems[i]) = sItems[i];
            for (i = SLOT_BOTTLE_1; i <= SLOT_BOTTLE_6; i++) {
                if (gSaveContext.save.saveInfo.inventory.items[i] == ITEM_NONE) {
                    gSaveContext.save.saveInfo.inventory.items[i] = ITEM_BOTTLE;
                }
            }
            PortCheat_FillAmmo();
            break;
        case CHEAT_GIVE_MASKS:
            for (i = ITEM_MASK_DEKU; i <= ITEM_MASK_GIANT; i++) INV_CONTENT(i) = i;
            break;
        case CHEAT_GIVE_SONGS:
            if (INV_CONTENT(ITEM_OCARINA_OF_TIME) != ITEM_OCARINA_OF_TIME) {
                INV_CONTENT(ITEM_OCARINA_OF_TIME) = ITEM_OCARINA_OF_TIME;
            }
            for (i = 0; i < ARRAY_COUNT(sSongs); i++) SET_QUEST_ITEM(sSongs[i]);
            break;
        case CHEAT_GIVE_UPGRADES:
            PortCheat_Upgrade(UPG_QUIVER, 3);
            PortCheat_Upgrade(UPG_BOMB_BAG, 3);
            PortCheat_Upgrade(UPG_WALLET, 2);
            PortCheat_Upgrade(UPG_DEKU_STICKS, 3);
            PortCheat_Upgrade(UPG_DEKU_NUTS, 3);
            if (GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD) != EQUIP_VALUE_SWORD_GILDED) Item_Give(play, ITEM_SWORD_GILDED);
            if (GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SHIELD) != EQUIP_VALUE_SHIELD_MIRROR) Item_Give(play, ITEM_SHIELD_MIRROR);
            gSaveContext.save.saveInfo.playerData.healthCapacity = 20 * 0x10;
            gSaveContext.save.saveInfo.playerData.health = 20 * 0x10;
            PortCheat_GiveMagic();
            if (!gSaveContext.save.saveInfo.playerData.isDoubleMagicAcquired) {
                gSaveContext.save.saveInfo.playerData.isDoubleMagicAcquired = true;
                gSaveContext.magicFillTarget = MAGIC_DOUBLE_METER;
                gSaveContext.save.saveInfo.playerData.magicLevel = 0;
            }
            gSaveContext.save.saveInfo.playerData.doubleDefense = true;
            gSaveContext.save.saveInfo.inventory.defenseHearts = 20;
            PortCheat_FillAmmo();
            gSaveContext.save.saveInfo.playerData.rupees = CUR_CAPACITY(UPG_WALLET);
            break;
        case CHEAT_GIVE_HEARTS: /* PORT (2026-11-08) */
            gSaveContext.save.saveInfo.playerData.healthCapacity = 20 * 0x10;
            gSaveContext.save.saveInfo.playerData.health = 20 * 0x10;
            gSaveContext.save.saveInfo.playerData.doubleDefense = true;
            gSaveContext.save.saveInfo.inventory.defenseHearts = 20;
            break;
        case CHEAT_GIVE_OWLS: /* the ten owl statues (OWL_WARP_GREAT_BAY_COAST .. OWL_WARP_STONE_TOWER) */
            for (i = OWL_WARP_GREAT_BAY_COAST; i <= OWL_WARP_STONE_TOWER; i++) SET_OWL_STATUE_ACTIVATED(i);
            break;
        case CHEAT_GIVE_MAP: /* every Tingle map: the world map's clouds and the minimaps */
            for (i = 0; i < TINGLE_MAP_MAX; i++) Inventory_SetWorldMapCloudVisibility(i);
            break;
        case CHEAT_GIVE_BOTTLES:
            for (i = SLOT_BOTTLE_1; i <= SLOT_BOTTLE_6; i++) gSaveContext.save.saveInfo.inventory.items[i] = ITEM_FAIRY;
            break;
        case CHEAT_GIVE_DAWN: /* 6:00 on the First Day, everything kept; the area is entered again so it is the morning */
            if ((play->pauseCtx.state != PAUSE_STATE_OFF) || (play->transitionTrigger != TRANS_TRIGGER_OFF) ||
                Player_InCsMode(play)) {
                Audio_PlaySfx(NA_SE_SY_ERROR);
                return;
            }
            gSaveContext.save.day = 1;
            gSaveContext.save.eventDayCount = 1;
            gSaveContext.save.time = CLOCK_TIME(6, 0) + 1;
            gSaveContext.save.isNight = false;
            gSaveContext.nextDayTime = NEXT_TIME_NONE;
            play->nextEntrance = gSaveContext.save.entrance;
            play->transitionTrigger = TRANS_TRIGGER_START;
            play->transitionType = TRANS_TYPE_FADE_WHITE;
            break;
        case CHEAT_GIVE_QUEST:
            SET_QUEST_ITEM(QUEST_REMAINS_ODOLWA);
            SET_QUEST_ITEM(QUEST_REMAINS_GOHT);
            SET_QUEST_ITEM(QUEST_REMAINS_GYORG);
            SET_QUEST_ITEM(QUEST_REMAINS_TWINMOLD);
            SET_QUEST_ITEM(QUEST_BOMBERS_NOTEBOOK);
            break;
        default:
            return;
    }
    /* the C buttons show the new items */
    for (i = EQUIP_SLOT_C_LEFT; i <= EQUIP_SLOT_C_RIGHT; i++) Interface_LoadItemIcon(play, i);
    Audio_PlaySfx(NA_SE_SY_GET_ITEM);
}

static int PortCheat_TimeStopHere(PlayState* play) {
    int mode = gPortCheats[CHEAT_TIMESTOP];

    if (mode >= 3) return true;
    switch (play->sceneId) {
        case SCENE_MITURIN: case SCENE_MITURIN_BS: case SCENE_HAKUGIN: case SCENE_HAKUGIN_BS:
        case SCENE_SEA: case SCENE_SEA_BS: case SCENE_INISIE_N: case SCENE_INISIE_R: case SCENE_INISIE_BS:
            return mode >= 1;
        case SCENE_KINSTA1: case SCENE_KINDAN2: case SCENE_KAIZOKU: case SCENE_PIRATE: case SCENE_TORIDE:
        case SCENE_REDEAD: case SCENE_CASTLE: case SCENE_IKNINSIDE: case SCENE_RANDOM:
            return mode >= 2;
        default:
            return false;
    }
}

static void PortCheat_Update(PlayState* play) {
    static s16 sStopScene = -1, sStopSpeed;
    SavePlayerData* pd = &gSaveContext.save.saveInfo.playerData;

    if (gPortCheatGive) {
        int what = gPortCheatGive;
        gPortCheatGive = 0;
        if (gSaveContext.gameMode == GAMEMODE_NORMAL) PortCheat_Give(play, what);
        else Audio_PlaySfx(NA_SE_SY_ERROR);
    }
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) return;

    if (gPortCheats[CHEAT_HEALTH] && pd->health > 0) pd->health = pd->healthCapacity;
    if (gPortCheats[CHEAT_MAGIC] && pd->isMagicAcquired) {
        if (pd->magicLevel == 1) pd->magic = MAGIC_NORMAL_METER;
        else if (pd->magicLevel == 2) pd->magic = MAGIC_DOUBLE_METER;
    }
    if (gPortCheats[CHEAT_RUPEES]) pd->rupees = CUR_CAPACITY(UPG_WALLET);
    if (gPortCheats[CHEAT_AMMO]) PortCheat_FillAmmo();
    if (gPortCheats[CHEAT_KEYS] && (gSaveContext.mapIndex >= 0) &&
        (gSaveContext.mapIndex < ARRAY_COUNT(gSaveContext.save.saveInfo.inventory.dungeonKeys)) &&
        (DUNGEON_KEY_COUNT(gSaveContext.mapIndex) < 9)) {
        DUNGEON_KEY_COUNT(gSaveContext.mapIndex) = 9;
    }
    /* time stop: remember the speed it replaced, and give it back when the cheat goes off in the same scene */
    if (sStopScene != play->sceneId) sStopScene = -1;
    if (gPortCheats[CHEAT_TIMESTOP] && PortCheat_TimeStopHere(play)) {
        if (R_TIME_SPEED != 0) {
            sStopScene = play->sceneId;
            sStopSpeed = R_TIME_SPEED;
            R_TIME_SPEED = 0;
        }
    } else if (sStopScene == play->sceneId) {
        if (R_TIME_SPEED == 0) R_TIME_SPEED = sStopSpeed;
        sStopScene = -1;
    }
    {
        extern f32 D_8085D958[2]; /* z_player.c: how far a Deku flower glide goes */
        D_8085D958[0] = gPortCheats[CHEAT_GLIDE] ? 99999.9f : 600.0f;
        D_8085D958[1] = gPortCheats[CHEAT_GLIDE] ? 99999.9f : 960.0f;
    }
}

/* ---------------- every game update (Play_Update) ---------------- */

void PortEnh_Update(PlayState* play) {
    Player* player = GET_PLAYER(play);

    gPortCanPauseSave = gPortPauseSave && IS_PAUSED(&play->pauseCtx) && (play->pauseCtx.state == PAUSE_STATE_MAIN) &&
                        PortEnh_CanSave(play);
    if (gPortPauseSaveReq) {
        gPortPauseSaveReq = 0;
        if (gPortCanPauseSave) {
            PortEnh_PauseSave(play);
        } else {
            Audio_PlaySfx(NA_SE_SY_ERROR);
            gPortPauseSaveResult = -1;
        }
    }
    if (gPortNotebookReq) { /* the Bomber's Notebook, as when used from a C button */
        gPortNotebookReq = 0;
        if (CHECK_QUEST_ITEM(QUEST_BOMBERS_NOTEBOOK) &&
            (!IS_PAUSED(&play->pauseCtx) || (play->pauseCtx.state == PAUSE_STATE_MAIN)) &&
            (player != NULL) && !Player_InBlockingCsMode(play, player) && (play->msgCtx.msgMode == MSGMODE_NONE) &&
            (gSaveContext.gameMode == GAMEMODE_NORMAL) && (play->transitionTrigger == TRANS_TRIGGER_OFF) &&
            !play->pauseCtx.bombersNotebookOpen) {
            play->pauseCtx.bombersNotebookOpen = true;
            Audio_PlaySfx(NA_SE_SY_DECIDE);
        } else {
            Audio_PlaySfx(NA_SE_SY_ERROR);
        }
    }
    PortDoubleTime_Update(play);
    PortCheat_Update(play);
}

/* the touch panel writes with the game's own font (3ds_main.c): the first n characters from ' ', 16x16 I4 each */
void Port_PanelFontLoad(unsigned char* dst, int n) {
    DmaMgr_RequestSync(dst, SEGMENT_ROM_START(nes_font_static), (size_t)n * FONT_CHAR_TEX_SIZE);
}
