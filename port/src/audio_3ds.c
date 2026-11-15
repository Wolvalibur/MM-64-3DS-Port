#include <3ds.h>
#include <string.h>

#define PORT_AUDIO_RATE   32000
#define PORT_AUDIO_NBUFS  8 /* (4 before the Old 3DS margin below, 6 before the New 3DS one: room for frames queued) */
#define PORT_AUDIO_MAXSAMPLES 1600 /* per submit; engine frames are ~544-736 */

static bool sNdspOk = false;
static ndspWaveBuf sWaveBufs[PORT_AUDIO_NBUFS];
static s16* sWaveData[PORT_AUDIO_NBUFS];
static int sNextBuf = 0;
u32 gPortPerfAudioUnderruns, gPortPerfAudioDrops; /* perf report: the DSP ran dry before a buffer came / a buffer dropped */
static int sAudioStarted;
/* PORT (2026-10-30): gap concealment. When the game falls behind (opening the pause screen unpacks ~80 icons, a scene
 * load, a heavy frame) the DSP ran out of samples: the sound stopped dead mid-wave and restarted mid-wave, two clicks
 * heard as a crack (user report: "random audio cracking, when I press pause"). A watchdog thread now sees the queue
 * about to run dry and queues a short fade from the last sample to silence, and the first buffer after a gap fades
 * in: a gap is a quick dip instead of a crack. */
#define PORT_AUDIO_FADE 192 /* samples (6 ms) */
static ndspWaveBuf sFadeBuf;
static s16* sFadeData;
static s16 sLastL, sLastR;          /* the last stereo sample submitted */
static volatile int sFadeIn;        /* the next submitted buffer starts from silence */
static LightLock sSubmitLock;
static Thread sWatchdog;
static void Port3ds_AudioWatchdog(void* arg);
static void Port3ds_AudioSubmitLocked(const s16* samples, int nsamples);

/* PORT (2026-11-14): diagnostic (HOME froze, user report: boot.log ended at "[apt] hook 0"). Going to the HOME menu, libctru
 * runs the APT hooks, then saves VRAM and hands the screen capture to the HOME menu, then puts the DSP to sleep (ndsp's
 * DSP hook waits for the DSP's answer, no timeout), then gives up the GPU. This hook runs just before ndsp's: the log
 * shows whether the switch got as far as the DSP (3ds_main.c Port3ds_FreezeWatch reports the step if it stops). */
static dspHookCookie sDspHook;
static void Port3ds_DspHook(DSP_HookType type) {
    extern void PortDbgX(const char* label, unsigned val);
    extern void Port3ds_LogFlush(void);
    extern volatile int gPortAptStep;
    if (type == DSPHOOK_ONSLEEP) gPortAptStep = 2;
    PortDbgX("[dsp] hook (0 sleep, 1 wakeup, 2 cancel)", (unsigned)type);
    Port3ds_LogFlush();
}

void Port3ds_AudioInit(void) {
    extern void PortDbg(const char*);
    if (R_FAILED(ndspInit())) {
        PortDbg("[audio] ndspInit FAILED - audio disabled");
        return;
    }
    PortDbg("[audio] ndspInit OK - ndsp up");
    dspHook(&sDspHook, Port3ds_DspHook); /* (after ndspInit: called before ndsp's own hook) */
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)PORT_AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < PORT_AUDIO_NBUFS; i++) {
        sWaveData[i] = (s16*)linearAlloc(PORT_AUDIO_MAXSAMPLES * 2 * sizeof(s16));
        if (sWaveData[i] == NULL) {
            PortDbg("[audio] no linear memory for DSP buffers - audio disabled");
            while (i > 0) {
                i--;
                linearFree(sWaveData[i]);
            }
            ndspExit();
            return;
        }
        memset(&sWaveBufs[i], 0, sizeof(ndspWaveBuf));
        sWaveBufs[i].status = NDSP_WBUF_DONE;
    }

    sFadeData = (s16*)linearAlloc(PORT_AUDIO_FADE * 2 * sizeof(s16));
    memset(&sFadeBuf, 0, sizeof(sFadeBuf));
    sFadeBuf.status = NDSP_WBUF_DONE;
    LightLock_Init(&sSubmitLock);

    sNdspOk = true;
    if (sFadeData != NULL) {
        bool n3ds = false;
        s32 prio = 0x30;
        APT_CheckNew3DS(&n3ds);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        /* a tiny loop (a status check every 2 ms) on the core the mixer uses, above it */
        sWatchdog = threadCreate(Port3ds_AudioWatchdog, NULL, 4 * 1024, prio > 0x19 ? prio - 2 : prio, n3ds ? 2 : 1, true);
    }
}

/* PORT (2026-10-26): after sleep (and the HOME menu) the DSP comes back with an empty queue, but our wave buffers kept
 * their QUEUED/PLAYING status: every new buffer was dropped as "all in flight", and the game stayed silent (user
 * report: no audio after waking the 3DS). The channel is cleared and set up again, and the buffers marked free. Called
 * on wake-up/restore (3ds_main.c) and when submits keep finding no free buffer. */
static int sDropRun;
void Port3ds_AudioResume(void) {
    int i;
    if (!sNdspOk) {
        return;
    }
    ndspChnWaveBufClear(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)PORT_AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    for (i = 0; i < PORT_AUDIO_NBUFS; i++) {
        sWaveBufs[i].status = NDSP_WBUF_DONE;
    }
    sFadeBuf.status = NDSP_WBUF_DONE;
    sFadeIn = 0;
    sNextBuf = 0;
    sAudioStarted = 0;
    sDropRun = 0;
}

int Port3ds_AudioReady(void) {
    return sNdspOk ? 1 : 0;
}

static int Port3ds_AudioLiveSamples(void) {
    int i, live = 0;
    for (i = 0; i < PORT_AUDIO_NBUFS; i++) {
        if (sWaveBufs[i].status == NDSP_WBUF_QUEUED || sWaveBufs[i].status == NDSP_WBUF_PLAYING) {
            live += (int)sWaveBufs[i].nsamples;
        }
    }
    if (live > 0) {
        live -= (int)ndspChnGetSamplePos(0);
    }
    return live;
}

static void Port3ds_AudioWatchdog(void* arg) {
    (void)arg;
    for (;;) {
        svcSleepThread(2000000LL);
        if (!sNdspOk || !sAudioStarted || sFadeIn) {
            continue; /* not playing yet, or this gap already faded */
        }
        LightLock_Lock(&sSubmitLock);
        if (sAudioStarted && !sFadeIn && Port3ds_AudioLiveSamples() < 96 &&
            (sFadeBuf.status == NDSP_WBUF_DONE || sFadeBuf.status == NDSP_WBUF_FREE)) {
            int i;
            for (i = 0; i < PORT_AUDIO_FADE; i++) {
                int k = PORT_AUDIO_FADE - 1 - i; /* PORT_AUDIO_FADE - 1 .. 0 */
                sFadeData[2 * i] = (s16)((int)sLastL * k / PORT_AUDIO_FADE);
                sFadeData[2 * i + 1] = (s16)((int)sLastR * k / PORT_AUDIO_FADE);
            }
            memset(&sFadeBuf, 0, sizeof(sFadeBuf));
            sFadeBuf.data_vaddr = sFadeData;
            sFadeBuf.nsamples = PORT_AUDIO_FADE;
            {
                extern void Port3ds_CacheFlush(const void* p, u32 size);
                Port3ds_CacheFlush(sFadeData, PORT_AUDIO_FADE * 2 * sizeof(s16));
            }
            ndspChnWaveBufAdd(0, &sFadeBuf);
            sFadeIn = 1;
        }
        LightLock_Unlock(&sSubmitLock);
    }
}

void Port3ds_AudioSubmit(const s16* samples, int nsamples) {
    if (!sNdspOk || samples == NULL || nsamples <= 0) {
        return;
    }
    LightLock_Lock(&sSubmitLock);
    Port3ds_AudioSubmitLocked(samples, nsamples);
    LightLock_Unlock(&sSubmitLock);
}

static void Port3ds_AudioSubmitLocked(const s16* samples, int nsamples) {
    if (nsamples > PORT_AUDIO_MAXSAMPLES) {
        nsamples = PORT_AUDIO_MAXSAMPLES;
    }
    ndspWaveBuf* wb = &sWaveBufs[sNextBuf];
    if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
        gPortPerfAudioDrops++;
        if (++sDropRun < 30) {
            return; /* all buffers in flight; drop */
        }
        Port3ds_AudioResume(); /* ~half a second without one free: the queue is stale (sleep), start it again */
        wb = &sWaveBufs[sNextBuf];
    }
    sDropRun = 0;
    {
        int i, live = 0;
        for (i = 0; i < PORT_AUDIO_NBUFS; i++) {
            live |= sWaveBufs[i].status == NDSP_WBUF_QUEUED || sWaveBufs[i].status == NDSP_WBUF_PLAYING;
        }
        gPortPerfAudioUnderruns += sAudioStarted && !live; /* a gap: the DSP had nothing left to play */
        if (sAudioStarted && !live) {
            sFadeIn = 1; /* (the watchdog missed it: still start from silence) */
        }
        sAudioStarted = 1;
    }
    memcpy(sWaveData[sNextBuf], samples, (size_t)nsamples * 2 * sizeof(s16));
    if (sFadeIn) { /* after a gap: in from silence, not mid-wave */
        s16* d = sWaveData[sNextBuf];
        int i, n = nsamples < PORT_AUDIO_FADE ? nsamples : PORT_AUDIO_FADE;
        for (i = 0; i < n; i++) {
            d[2 * i] = (s16)((int)d[2 * i] * i / n);
            d[2 * i + 1] = (s16)((int)d[2 * i + 1] * i / n);
        }
        sFadeIn = 0;
    }
    sLastL = sWaveData[sNextBuf][2 * (nsamples - 1)];
    sLastR = sWaveData[sNextBuf][2 * (nsamples - 1) + 1];
    memset(wb, 0, sizeof(ndspWaveBuf));
    wb->data_vaddr = sWaveData[sNextBuf];
    wb->nsamples = (u32)nsamples;
    {
        extern void Port3ds_CacheFlush(const void* p, u32 size); /* gfx_3ds.c: the kernel, not the DSP service */
        Port3ds_CacheFlush(sWaveData[sNextBuf], (u32)((size_t)nsamples * 2 * sizeof(s16)));
    }
    ndspChnWaveBufAdd(0, wb);
    sNextBuf = (sNextBuf + 1) % PORT_AUDIO_NBUFS;
}

/* PORT (2026-10-05): samples the engine is told are NOT queued (osAiGetLength), so it keeps that much more queued.
 * The engine aims at its frame plus 128 samples (4 ms) still queued when the next frame arrives - enough for the N64,
 * whose audio task runs on time. On an Old 3DS the mixer shares the system core (7.5-10 ms per task in big scenes,
 * hardware v58) and the game's update can hold the next task back: a frame more than 4 ms late found the DSP empty
 * and played a gap (crackling, user report). With one more frame queued (~16 ms more latency) it tolerates ~20 ms.
 * -1 = by model (Old 3DS mixer on the system core: 512, else 256); settings audio_margin=<samples>, or the touch
 * panel's settings page (Audio buffer: SMALL 0, MEDIUM 256, LARGE 512).
 * PORT (2026-10-26): the New 3DS default was 0, and slight crackling was reported there too (user report): with only
 * the engine's 4 ms of slack, an update that ran a few ms late (a big scene, a scene load) still found the DSP empty.
 * 256 samples (8 ms more latency) covers those; LARGE (512) is there for scenes that still crackle. */
int gPortAudioMargin = -1;

/* Stereo frames queued on ndsp and not yet played (the N64 osAiGetLength equivalent). */
int Port3ds_AudioQueuedFrames(void) {
    int i, queued = 0;
    if (!sNdspOk) {
        return 0;
    }
    for (i = 0; i < PORT_AUDIO_NBUFS; i++) {
        if (sWaveBufs[i].status == NDSP_WBUF_QUEUED || sWaveBufs[i].status == NDSP_WBUF_PLAYING) {
            queued += (int)sWaveBufs[i].nsamples;
        }
    }
    if (queued > 0) {
        queued -= (int)ndspChnGetSamplePos(0); /* position within the buffer now playing */
    }
    {
        extern int gPortAudioCoreNow;
        /* PORT (2026-10-30): AUTO is 512 (16 ms) on both models now: 256 still cracked when opening the pause screen.
         * PORT (2026-11-06): AUTO is 1024 (32 ms): with 512 an occasional stutter was still heard in normal play (user
         * report) - an update running more than ~20 ms late still emptied the DSP queue. 32 ms more slack covers a
         * late frame of the 20 fps game; the added latency (~16 ms) is below what is noticed against the picture.
         * Queued at most: frame (~740) + engine slack (128) + 1024 = ~1900 samples, 3 of the 8 DSP buffers. */
        int margin = gPortAudioMargin >= 0 ? gPortAudioMargin : 1024;
        queued -= margin;
    }
    return queued > 0 ? queued : 0;
}

/* PORT (2026-09-29): the audio "RSP". On the N64 the RSP runs each audio task while the CPU builds the next
 * one (AudioMgr_HandleRetrace dispatches task N, runs AudioThread_Update for N+1, then waits for N-1); the
 * engine's DMA buffer lifetimes assume that overlap. Here the C microcode (PortAudio_RunTask) runs on a
 * worker thread on another core - core 2 on New 3DS, the system core 1 (with an app CPU-time allowance) on
 * Old 3DS - so it no longer eats the main thread's frame time. Dispatching a task first waits for the
 * previous one, so tasks still run in order. If no worker can be created, tasks run synchronously. */
/* OSTask (ultra64.h, which clashes with <3ds.h>) copied as opaque bytes; sched_shim.c checks the size */
#define PORT_OSTASK_SIZE 64
typedef struct { u8 bytes[PORT_OSTASK_SIZE]; } PortOSTask;
extern void PortAudio_RunTask(void* task);

static Thread sAudioWorker;
static LightEvent sAudioJobStart, sAudioJobDone;

static PortOSTask sAudioJob __attribute__((aligned(8)));
static volatile bool sAudioJobBusy;
static int sAudioAsync = -1; /* -1 = not tried yet */

u64 gPortPerfAudioUcode, gPortPerfAudioWait; /* ticks: microcode (worker core), main thread waiting for it */
u32 gPortPerfAudioTasks;

static void Port3ds_AudioWorkerMain(void* arg) {
    (void)arg;
    for (;;) {
        u64 t0;
        LightEvent_Wait(&sAudioJobStart);
        t0 = svcGetSystemTick();
        PortAudio_RunTask(&sAudioJob);
        gPortPerfAudioUcode += svcGetSystemTick() - t0;
        gPortPerfAudioTasks++;
        sAudioJobBusy = false;
        LightEvent_Signal(&sAudioJobDone);
    }
}

int gPortAudioCore = -1; /* -1: by model (New 3DS core 2, Old 3DS core 1) */
unsigned gPortAudioCore1Limit; /* the system core share granted (perf report), 0 when not on core 1 */
int gPortAudioCoreNow = -1;    /* the core running the current tasks (perf report) */
/* audio_share_ab (3ds_main.c): a new system-core share between perf reports */
void Port3ds_AudioSetShare(int percent) {
    if (gPortAudioCore1Limit != 0 && R_SUCCEEDED(APT_SetAppCpuTimeLimit((u32)percent))) {
        gPortAudioCore1Limit = (unsigned)percent;
    }
}

/* Old 3DS: the audio microcode needs a task every retrace, 2-4 ms each at 268 MHz (hardware v49 on core 2). The
 * system core gives an application only the share it asked for with APT_SetAppCpuTimeLimit, up to the exheader's
 * limit (port/oot.rsf MaxCpu, 89%). Measured at Old 3DS speed (PORT 2026-10-04):
 *   30% (v52; the default exheader limit refused more): 12-18 ms per task, the game waited for it - low frame rate,
 *       crackling audio;
 *   80% (v54): 5-6.7 ms per task, but every request to the system services that live on that core (the DSP's cache
 *       flush per audio pump, the GPU's) got slower - the audio engine on the game thread went from ~3 to 10-14 ms
 *       per update, on a New 3DS too, where the mixer was not even on core 1 (the share was reserved for perf_ab).
 * So: never on a New 3DS (core 2 is free), and a middle share on an Old 3DS until hardware decides
 * (settings audio_share=N, audio_share_ab=1 cycles 30/55/80 for a measurement). */
int gPortAudioShare = 55;
static void Port3ds_AudioAskSystemCore(void) {
    u32 want = (u32)(gPortAudioShare < 10 ? 10 : gPortAudioShare > 89 ? 89 : gPortAudioShare);
    u32 limits[4] = { want, 55, 30, 0 }, granted = 0;
    int i;
    for (i = 0; i < 3; i++) {
        if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(limits[i]))) {
            break;
        }
    }
    APT_GetAppCpuTimeLimit(&granted);
    gPortAudioCore1Limit = granted;
    {
        extern void PortDbgX(const char* label, unsigned val);
        PortDbgX("[audio] system core share for the mixer (%)", granted);
    }
}
static void Port3ds_AudioWorkerStart(void) {
    extern void PortDbg(const char*);
    bool n3ds = false;
    s32 prio = 0x30;
    int core;

    sAudioAsync = 0;
    LightEvent_Init(&sAudioJobStart, RESET_ONESHOT);
    LightEvent_Init(&sAudioJobDone, RESET_ONESHOT);
    APT_CheckNew3DS(&n3ds);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    core = n3ds ? 2 : 1;
    if (gPortAudioCore >= 0) {
        core = gPortAudioCore;
    }
    if (core == 1) {
        Port3ds_AudioAskSystemCore();
    }
    sAudioWorker = threadCreate(Port3ds_AudioWorkerMain, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : prio, core, true);
    if (sAudioWorker == NULL && n3ds) {
        APT_SetAppCpuTimeLimit(30);
        core = 1;
        sAudioWorker = threadCreate(Port3ds_AudioWorkerMain, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : prio, core, true);
    }
    gPortAudioCoreNow = sAudioWorker != NULL ? core : 0;
    if (sAudioWorker != NULL) {
        sAudioAsync = 1;
        PortDbg(core == 2 ? "[audio] microcode worker on core 2" : core == 1 ? "[audio] microcode worker on core 1" :
                                                                             "[audio] microcode worker on core 0");
    } else {
        PortDbg("[audio] no microcode worker thread - running audio tasks synchronously");
    }
}

/* closing (gfx_3ds.c): the previous task, but never forever - a stuck wait kept the software from closing and the
 * HOME menu hung with it (user report) */
void Port3ds_AudioTaskWaitExit(void) {
    u64 t0 = svcGetSystemTick();
    while (sAudioJobBusy && svcGetSystemTick() - t0 < SYSCLOCK_ARM11 / 2) {
        LightEvent_WaitTimeout(&sAudioJobDone, 10000000LL);
    }
    if (sNdspOk) {
        ndspChnWaveBufClear(0);
    }
}

/* (the freeze detector, 3ds_main.c: an audio task still running) */
int Port3ds_AudioJobPending(void) {
    return sAudioJobBusy;
}

/* wait until the previous audio task has finished */
void Port3ds_AudioTaskWait(void) {
    u64 t0 = svcGetSystemTick();
    while (sAudioJobBusy) {
        LightEvent_Wait(&sAudioJobDone);
    }
    gPortPerfAudioWait += svcGetSystemTick() - t0;
}

/* run (or start) one audio task; returns once it may be treated as complete by the scheduler */
void Port3ds_AudioTaskRun(void* task) {
    if (sAudioAsync < 0) {
        Port3ds_AudioWorkerStart();
    }
    if (sAudioAsync == 0) {
        u64 t0 = svcGetSystemTick();
        PortAudio_RunTask(task);
        gPortPerfAudioUcode += svcGetSystemTick() - t0;
        gPortPerfAudioTasks++;
        return;
    }
    Port3ds_AudioTaskWait();
    memcpy(&sAudioJob, task, sizeof(sAudioJob)); /* the command list stays valid: the engine double-buffers it */
    sAudioJobBusy = true;
    LightEvent_Signal(&sAudioJobStart);
}
