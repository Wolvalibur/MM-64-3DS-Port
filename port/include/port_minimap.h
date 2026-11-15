#ifndef PORT_MINIMAP_H
#define PORT_MINIMAP_H

#define PORT_MINIMAP_MAX_MARKS 32
#define PORT_MINIMAP_MAX_LAYERS 2

typedef struct {
    const void* tex;
    short x, y, w, h;
    unsigned char fmt, flipX, flipY, byteXor;
    unsigned char r, g, b, a;
} PortMinimapLayer;

typedef struct {
    unsigned int serial;
    int sceneId;
    unsigned char numLayers;
    PortMinimapLayer layers[PORT_MINIMAP_MAX_LAYERS];
    unsigned char compass, hasStart;
    float playerX, playerY; /* arrow centers on the N64 screen */
    short playerYaw;
    float startX, startY;
    short startYaw;
    unsigned char numMarks;
    unsigned char markType[PORT_MINIMAP_MAX_MARKS];
    short markX[PORT_MINIMAP_MAX_MARKS], markY[PORT_MINIMAP_MAX_MARKS];
    unsigned char markR[PORT_MINIMAP_MAX_MARKS], markG[PORT_MINIMAP_MAX_MARKS], markB[PORT_MINIMAP_MAX_MARKS];
} PortMinimap;

#define PORT_MINIMAP_I4 1
#define PORT_MINIMAP_IA4 2
#define PORT_MINIMAP_I4_ALPHA 3

/* PORT (2026-10-26): widescreen HUD anchoring (gfx_pc.c gfx_hud_shift_px). Tags a HUD group in the overlay display
 * list: PORT_HUD_LEFT / PORT_HUD_RIGHT move its draws to that screen edge in widescreen, PORT_HUD_NONE ends it. */
#define PORT_HUD_NONE 0
#define PORT_HUD_LEFT 1
#define PORT_HUD_RIGHT 2
#define PORT_HUD_LEFT_BOTTOM 3 /* (2026-11-12) the same edges, naming the corner the HUD size setting scales toward */
#define PORT_HUD_RIGHT_BOTTOM 4
#define PORT_HUD_BOTTOM 5 /* (2026-11-14) the clock: no widescreen move, scaled toward the bottom edge's middle */
#define gPortHudAnchor(pkt, anchor)                                     \
    do {                                                                \
        Gfx* _g = (Gfx*)(pkt);                                          \
        _g->words.w0 = (unsigned)(unsigned char)G_NOOP << 24;           \
        _g->words.w1 = 0x3D5E4100u | (unsigned)(anchor);                \
    } while (0)

extern PortMinimap gPortMinimap;
extern int gPortMinimapOnBottom; /* 1 = the touch panel shows the map; the top screen does not */

extern const unsigned char* gPortHudIconSeg;
extern unsigned int gPortHudSerial;           /* bumped by every Interface_Draw */
extern int gPortHudKeys;                      /* small keys shown by the HUD, -1 = none */
extern int gPortHudTatl;
extern int gPortHudTop;                       /* 1 = the N64 HUD on the top screen (default), 0 = hidden */
extern volatile int gPortTouchOcarina;
extern volatile int gPortTouchPage; /* pause page for the next START (-1 = the game's own), z_kaleido_setup.c */

typedef struct {
    int valid; /* a save is loaded and the game is in normal play mode */
    int rupees, keys;
    int health, healthCapacity; /* 16 per heart */
    int magic, magicCapacity;
    int mask, ocarina;
    int day, minutes;
    unsigned char cItem[3], cDisabled[3];
    short cAmmo[3]; /* -1 = no counter */
    int cutscene;   /* a cutscene is playing (Play_InCsMode): the touch panel dims (3ds_main.c) */
    int notebook;   /* the Bomber's Notebook is owned (its tab on the touch panel) */
    /* PORT (2026-11-06): the MM3D-style touch panel's pages (3ds_main.c) */
    unsigned char inv[48];     /* the Items (0..23) and Masks (24..47) pages; ITEM_NONE 0xFF (masks given on the moon too) */
    short invAmmo[24];         /* the Items page's counters, -1 = none */
    unsigned char invDim[24];  /* the item can't be used in Link's current form (drawn grey, as on the pause screen) */
    unsigned char cSlot[3];    /* the inventory slot on each C button (SLOT_NONE 0xFF) */
    unsigned char remains;     /* bit k: boss remains k (Odolwa, Goht, Gyorg, Twinmold) */
    unsigned char songs;       /* the ten songs learned */
    char name[9];              /* the player's name */
    /* the counters' colour as the HUD's: green at the most it can hold (Interface_DrawAmmoCount) */
    unsigned char cAmmoFull[3], invAmmoFull[24];
    /* the beating heart (z_lifemeter.c LifeMeter_Draw): its size step 0..10 and colours, as the HUD draws them */
    unsigned char lifeSize, beatPrim[3], beatEnv[3];
    /* the rupee counter (z_parameter.c Interface_Draw): the wallet (0..2: its digits and its icon's colour), held at its most */
    unsigned char wallet, rupeesFull;
    /* the double defense hearts: the first defHearts hearts (inventory.defenseHearts, z_lifemeter.c) */
    unsigned char defHearts;
} PortHudInfo;
/* PORT (2026-11-06): equipping from the touch panel (game thread, PortMaskSlot_Update): (C button 1..3 << 8) | (slot + 1);
 * a mask for the II (ZR) slot: item + 1 */
extern volatile int gPortEquipReq, gPortMaskSlotReq;
const unsigned char* Port_GetItemIconCached(int itemId);
void Port_GetHudInfo(PortHudInfo* h);
struct PlayState;
void PortHud_Update(struct PlayState* play);
/* PORT (2026-10-26): the ZR mask slot. gPortMaskSlotItem: the mask on ZR (ITEM_NONE 0xFF: none), assigned on the pause
 * screen's Masks page by pressing ZR on a mask (gPortMaskAssignReq, from 3ds_main.c); in gameplay ZR presses
 * PORT_BTN_MASK, an N64 controller bit the game never reads otherwise, and the player puts the mask on or takes it off
 * (z_player.c). gPortInPause: the pause screen is open (ZR assigns there). */
#define PORT_BTN_MASK 0x0040
extern int gPortMaskSlotItem;
/* PORT (2026-11-12): D-pad items, Project Restoration's layout. In gameplay the D-pad presses PORT_BTN_DPAD (another
 * controller bit the game never reads) with gPortDpadDir = 1 up (Goron Mask), 2 down (Deku Mask), 3 left (Zora Mask),
 * 4 right (ocarina), 5 ZR + up (Fierce Deity's Mask); z_player.c uses the item where the game would allow it on a C
 * button. While a message or the ocarina staff is up (gPortDpadAsC, set by the game each update) the D-pad stays the
 * C buttons, for the notes; the Gameplay row "D-pad C buttons" (ENH_DPAD_CBTNS) keeps that layout everywhere. */
#define PORT_BTN_DPAD 0x0080
extern int gPortDpadDir, gPortDpadAsC;
#define PORT_MASK_SLOT_FILES 4
extern int gPortMaskSlotFile[PORT_MASK_SLOT_FILES]; /* the ZR mask of each save file (ultra_shims.c) */
extern volatile unsigned gPortMaskSlotSerial;
extern volatile int gPortMaskAssignReq;
extern volatile int gPortInPause;
void PortMaskSlot_Update(struct PlayState* play);
void PortMap_Begin(int sceneId);
void PortMap_AddMark(int type, int x, int y, int r, int g, int b);


/* ---- quality-of-life options (ultra_shims.c) ---- */
/* PORT (2026-10-26): quality-of-life options from 2Ship2Harkinian (mm/2s2h/Enhancements), switched on the touch panel's
 * SETUP page, GAMEPLAY tab (3ds_main.c), saved in settings.txt. 0 = the original game. */
#ifdef __3DS__
extern int gPortFastText;       /* text prints instantly, holding B skips through it (Dialogue/FastText) */
extern int gPortSkipCutscenes;  /* skip the intro and the repeated Song of Time / Soaring cutscenes */
extern int gPortInstantPutaway; /* items and the sword are put away at once (Player/InstantPutaway) */
extern int gPortArrowCycle;     /* L while aiming the bow cycles normal / fire / ice / light arrows */
extern int gPortOwlPersist;     /* loading an owl save does not delete it (Saving/SavingEnhancements) */
extern int gPortDoubleTime;     /* Song of Double Time: choose the hour (Songs/BetterSongOfDoubleTime) */
extern int gPortPauseSave;
extern int gPortGuardSight; /* Deku guards' sight balls: 0 night only (original), 1 never, 2 always */
/* PORT (2026-11-08): more of 2Ship2Harkinian's options (GAMEPLAY tab, pages 1-3), all off by default. Saved in
 * settings.txt as enh<index>=1, so new ones go at the end. */
enum {
    ENH_FAST_CHESTS,      /* chests open without the long lift (Timesavers/FastChests) */
    ENH_FAST_SONGS,       /* songs take effect without the replay (Songs/FasterSongPlayback) */
    ENH_FAST_RUPEES,      /* the rupee counter fills / drains in a moment (Timesavers/FasterRupeeAccumulator) */
    ENH_FAST_TRANSITIONS, /* the quick fades between areas (Timesavers/FasterSceneTransitions) */
    ENH_FAST_TRANSFORM,   /* the transformation masks without their cutscene (Masks/FastTransformation) */
    ENH_BLAST_COOLDOWN,   /* the Blast Mask is ready again at once (Masks/BlastMaskCooldown) */
    ENH_CLIMB_SPEED,      /* climbing at twice the speed (Player/ClimbSpeed) */
    ENH_FAST_PUSH,        /* blocks are pushed and pulled faster (Player/FasterPushAndPull) */
    ENH_UNDERWATER_OCARINA, /* Zora Link plays his guitar on the bottom of the water (Player/UnderwaterOcarina) */
    ENH_SIDE_ROLL,        /* the side roll and the power crouch stab (Restorations/SideRoll, PowerCrouchStab) */
    ENH_OWL_WARP,         /* warp to an owl statue from the pause map (Songs/PauseOwlWarp) */
    ENH_SUNS_SONG,        /* Sun's Song can be played (Songs/EnableSunsSong) */
    ENH_NO_TAKKURI,       /* Takkuri doesn't steal (DifficultyOptions/DisableTakkuriSteal) */
    ENH_NO_QUAKES,        /* no earthquakes on the final day (Accessibility/NoFinalDayQuakes) */
    ENH_WIDE_CULLING,     /* PORT (2026-11-10): with Widescreen on, actors in the extra side areas update and draw
                           * instead of being culled as off screen (z_actor.c Actor_CullingVolumeTest) */
    ENH_ACTOR_DISTANCE,   /* PORT (2026-11-12): actors draw and update farther away: 0 = off, 1..4 = 2x..5x the culling volume's
                           * forward distance (z_actor.c Actor_CullingVolumeTest; 2Ship2Harkinian's actor draw distance) */
    ENH_DPAD_CBTNS,       /* PORT (2026-11-12): the D-pad is the four C buttons, as before the D-pad items (PORT_BTN_DPAD) */
    ENH_HUD_SIZE,         /* PORT (2026-11-12): HUD size, 0..6 = 100 110 120 130 70 80 90 % (gfx_pc.c hud_scale) */
    ENH_CRT,              /* PORT (2026-11-12): CRT scanlines, 0 off, 1 light, 2 strong (gfx_pc.c gfx_crt_overlay) */
    ENH_INVERT_AIM_Y,     /* PORT (2026-11-14): first-person looking / aiming: the stick's up and down swapped (z_player.c
                           * func_80847190; 2Ship2Harkinian's Invert Aiming Y Axis). The gyro is not affected. */
    ENH_INVERT_AIM_X,     /* the same for left and right (Invert Aiming X Axis) */
    ENH_BOTTOM_OFF,       /* PORT (2026-11-14): the bottom screen off (black, backlight off, not drawn); a tap on it turns it
                           * back on (3ds_main.c Port3ds_TouchUiPoll). Shown as "Bottom screen ON/OFF", so 1 is OFF. */
    ENH_COUNT
};
extern int gPortEnh[ENH_COUNT];
#define PORT_ENH(e) (gPortEnh[e] != 0)
/* PORT (2026-10-28): cheats (SETUP page, CHEATS tab), all off by default; from 2Ship2Harkinian's Enhancements/Cheats
 * plus the usual give-all codes. Toggles are saved in settings.txt; the GIVE rows are one-shot requests that
 * PortEnh_Update carries out on the game thread. */
enum {
    CHEAT_HEALTH,      /* health stays full and hits take none */
    CHEAT_MAGIC,       /* infinite magic */
    CHEAT_RUPEES,      /* the wallet stays full */
    CHEAT_AMMO,        /* arrows, bombs, bombchus, sticks, nuts, beans, powder kegs stay full */
    CHEAT_KEYS,        /* nine small keys in the current dungeon */
    CHEAT_MOONJUMP,    /* hold L + R to float up */
    CHEAT_TIMESTOP,    /* 0 off, 1 temples, 2 temples + dungeons, 3 everywhere */
    CHEAT_RAZOR,       /* the Razor Sword never dulls */
    CHEAT_CLIMB,       /* every wall is climbable */
    CHEAT_HOOKSHOT,    /* the hookshot sticks to every surface */
    CHEAT_ELEGY,       /* Elegy of Emptiness works everywhere */
    CHEAT_UNRESTRICT,  /* every item in every form */
    CHEAT_CARROTS,     /* Epona's carrots never run out */
    CHEAT_GLIDE,       /* Deku flower glides last until you land */
    CHEAT_FIERCE,      /* Fierce Deity's Mask outside the boss rooms */
    /* PORT (2026-11-08): more (saved by index, so new ones go at the end) */
    CHEAT_NOCLIP,      /* Link floats through walls: stick moves, A up, B down, R faster (2S2H/SoH noclip) */
    CHEAT_DEKU_HOPS,   /* Deku Link hops across water without end (2S2H Player/InfiniteDekuHopping) */
    CHEAT_GORON_MAGIC, /* the spiked Goron roll uses no magic (2S2H Masks/GoronRollingIgnoresMagic) */
    CHEAT_MINIGAMES,   /* the minigames are won (2S2H Minigames, set to their easiest) */
    CHEAT_GROTTOS,     /* hidden grottos sparkle (2S2H DifficultyOptions/HiddenGrottosVisibility) */
    CHEAT_TOGGLES
};
enum { CHEAT_GIVE_ITEMS = 1, CHEAT_GIVE_MASKS, CHEAT_GIVE_SONGS, CHEAT_GIVE_UPGRADES, CHEAT_GIVE_QUEST,
       CHEAT_GIVE_HEARTS, CHEAT_GIVE_OWLS, CHEAT_GIVE_MAP, CHEAT_GIVE_BOTTLES, CHEAT_GIVE_DAWN };
extern int gPortCheats[CHEAT_TOGGLES];
extern volatile int gPortCheatGive; /* a CHEAT_GIVE_* request from the touch panel, 0 when none */
#define PORT_CHEAT_ON(c) (gPortCheats[c] != 0)
extern volatile int gPortPauseSaveReq, gPortPauseSaveResult, gPortNotebookReq, gPortCanPauseSave;
struct PlayState;
void PortEnh_Update(struct PlayState* play);
void PortEnh_TransitionCs(struct PlayState* play);
int PortEnh_PauseSaveLoadEntrance(void);
int PortDoubleTime_Begin(struct PlayState* play);
int PortDoubleTime_Active(void);
int PortDoubleTime_Pending(void);
void PortDoubleTime_ClockBegin(struct PlayState* play);
void PortDoubleTime_ClockEnd(struct PlayState* play);
void PortDoubleTime_CutsceneDone(struct PlayState* play);
void PortDoubleTime_PlayDestroy(void);
void PortSram_DeleteKeptOwlSave(struct PlayState* play);
#else
#define gPortFastText 0
#define gPortSkipCutscenes 0
#define gPortInstantPutaway 0
#define gPortArrowCycle 0
#define gPortOwlPersist 0
#define gPortDoubleTime 0
#define gPortPauseSave 0
#define gPortGuardSight 0
#define PORT_CHEAT_ON(c) 0
#define PortDoubleTime_Begin(play) 0
#endif


#endif
