/*
 * src/n64/audio.c
 *
 * N64 port — Audio Interface (AI) driver + M4A low-level mixer replacement
 *
 * ---------------------------------------------------------------------
 * THE PORT IS SILENT, AND THIS IS WHY
 *
 * Everything below the sequencer works: the AI is configured, double
 * buffering runs off the AI interrupt, and MixAudioFrame() will resample
 * and mix every active PCM and CGB channel it is given.
 *
 * Nothing ever gives it one. MixAudioFrame() reads its channels out of
 * SoundInfo, which is the M4A engine's state, and the M4A engine is not in
 * this build. The plan was to keep the high-level sequencer from src/m4a.c
 * and replace only the ARM assembly mixer in src/m4a_1.s -- but both files
 * are in EXCLUDED_SRCS in Makefile.n64, and what stands in for them is the
 * block of stubs at the bottom of this file. m4aSongNumStart() does
 * nothing, MPlayMain() does nothing, ply_note() does nothing, so no channel
 * is ever started and every frame mixes to silence.
 *
 * Sound therefore needs the sequencer written, not a bug fixed: MPlayMain's
 * per-track command walk, the ply_* command handlers, ply_note's channel
 * allocation, TrkVolPitSet, and the envelope stepping that SoundMain does
 * on GBA. That is the substance of m4a_1.s, in C.
 * ---------------------------------------------------------------------
 *
 * What is implemented here:
 *       1. Mixes all active PCM channels into a stereo 16-bit buffer.
 *       2. Mixes the 4 CGB channels (square, noise) via software synthesis.
 *       3. Feeds the buffer to the N64 AI (Audio Interface) via DMA.
 *
 * Audio parameters:
 *   Sample rate: 32000 Hz (upgraded from GBA's ~13379 Hz for better quality)
 *   Format:      16-bit signed stereo (N64 AI requirement)
 *   Buffer size: 1024 stereo samples per AI DMA buffer = 4096 bytes
 *   Double-buffered so one buffer plays while the other is being filled.
 *
 * GBA M4A PCM channel format:
 *   Samples are 8-bit signed at the instrument's native sample rate.
 *   Each channel has a volume (0-255), panning (0=left, 127=centre, 255=right),
 *   pitch (frequency in Hz), and loop/envelope state.
 *
 * GBA CGB channel emulation:
 *   Square wave channels (CH1/CH2): variable duty cycle, envelope, sweep.
 *   Wave channel (CH3): 32 4-bit samples from wave RAM, swept by frequency.
 *   Noise channel (CH4): LFSR-based pseudo-random noise.
 *   These are synthesised in software and mixed into the PCM buffer.
 */

#include <string.h>
#include <math.h>
#include "global.h"
#include "m4a.h"
#include "gba/m4a_internal.h"
#include "n64/asm_defs.h"

/* -----------------------------------------------------------------------
 * N64 AI register access — byte-swap wrappers for big-endian MMIO
 * --------------------------------------------------------------------- */
#define AI_REG_WR(off, val) N64_HW_WR(N64_AI_BASE_REG, (off), (val))
#define AI_REG_RD(off)      N64_HW_RD(N64_AI_BASE_REG, (off))

/* -----------------------------------------------------------------------
 * Audio buffer configuration
 * --------------------------------------------------------------------- */
#define N64_AUDIO_SAMPLE_RATE   32000
#define N64_AUDIO_SAMPLES_PER_BUF 1024
#define N64_AUDIO_BUF_BYTES     (N64_AUDIO_SAMPLES_PER_BUF * 2 * sizeof(s16))
/* Two buffers for double-buffering */
#define N64_AUDIO_NUM_BUFS      2

/* Audio buffers — 8-byte aligned for AI DMA */
static s16 sAudioBufs[N64_AUDIO_NUM_BUFS][N64_AUDIO_SAMPLES_PER_BUF * 2]
    __attribute__((aligned(8)));

static int sFillBuf  = 0;   /* index of buffer currently being filled */
static int sPlayBuf  = 1;   /* index of buffer currently playing      */
static volatile int sAIBusy = 0;  /* 1 = AI DMA in progress           */

/* -----------------------------------------------------------------------
 * GBA master volume (set by REG_SOUNDCNT_L / REG_SOUNDCNT_H)
 * --------------------------------------------------------------------- */
static inline int GetMasterVolume(void)
{
    /* GBA: SOUNDCNT_L bits 6-4 = output volume (0-7) for left/right */
    u16 cntH = _REG16(REG_OFFSET_SOUNDCNT_H);
    int vol = (cntH >> 2) & 3;  /* 0 = 25%, 1 = 50%, 2 = 100% */
    return vol;  /* used as a shift: 0→>>2, 1→>>1, 2→>>0 */
}

/* -----------------------------------------------------------------------
 * SoundInfo — the M4A engine writes here; we read channel state from it.
 * The pointer is set by m4aSoundInit() via SOUND_INFO_PTR.
 * --------------------------------------------------------------------- */
static inline struct SoundInfo *GetSoundInfo(void)
{
    return SOUND_INFO_PTR;
}

/* -----------------------------------------------------------------------
 * GBA M4A channel structure — mirrors m4a_internal.h SoundChannel
 *
 * We use the actual struct SoundChannel from gba/m4a_internal.h.
 * --------------------------------------------------------------------- */
typedef struct SoundChannel M4AChannel;

/* Map audio.c field aliases to the actual m4a_internal.h field names */
#define M4A_MAX_CHANNELS    MAX_DIRECTSOUND_CHANNELS
#define M4A_STATUS_ACTIVE   SOUND_CHANNEL_SF_START
#define M4A_STATUS_LOOP     SOUND_CHANNEL_SF_LOOP
#define M4A_TYPE_CGB        TONEDATA_TYPE_CGB

/* -----------------------------------------------------------------------
 * CGB channel state
 * --------------------------------------------------------------------- */
typedef struct {
    int     active;
    int     type;       /* 1=square1, 2=square2, 3=wave, 4=noise */
    int     duty;       /* square wave duty cycle (0-3) */
    int     envVol;     /* envelope volume (0-15) */
    int     envDir;     /* +1 = increase, -1 = decrease */
    int     envSteps;   /* envelope step count */
    int     envTimer;   /* envelope timer */
    int     freq;       /* frequency (Hz) */
    u32     phase;      /* oscillator phase (fixed-point) */
    u32     phaseInc;   /* phase increment per sample */
    int     lfsrState;  /* noise LFSR (15-bit) */
    u8      waveRAM[16];/* CH3 wave RAM */
} CgbChannel;

static CgbChannel sCgbChannels[4];

/* Duty cycle tables (fraction of period that output is high) */
static const float sDutyCycle[4] = {0.125f, 0.25f, 0.5f, 0.75f};

/* -----------------------------------------------------------------------
 * Update CGB channels from NR1x-NR4x registers
 * --------------------------------------------------------------------- */
static void UpdateCgbChannels(void)
{
    /* Square 1 (CH1) */
    {
        u8 nr10 = _REG8(REG_OFFSET_SOUND1CNT_L);
        u8 nr11 = _REG8(REG_OFFSET_SOUND1CNT_H);
        u8 nr12 = _REG8(REG_OFFSET_SOUND1CNT_H + 1);
        u8 nr14 = _REG8(REG_OFFSET_SOUND1CNT_X + 1);
        (void)nr10; (void)nr11;
        CgbChannel *c = &sCgbChannels[0];
        if (nr14 & 0x80) {  /* trigger */
            c->active   = 1;
            c->type     = 1;
            c->envVol   = (nr12 >> 4) & 0xF;
            c->envDir   = (nr12 & 0x08) ? 1 : -1;
            c->envSteps = nr12 & 0x07;
            c->envTimer = c->envSteps;
        }
        c->active = (c->active && c->envVol > 0);
    }

    /* Square 2 (CH2) */
    {
        u8 nr21 = _REG8(REG_OFFSET_SOUND2CNT_L);
        u8 nr22 = _REG8(REG_OFFSET_SOUND2CNT_L + 1);
        u8 nr24 = _REG8(REG_OFFSET_SOUND2CNT_H + 1);
        (void)nr21;
        CgbChannel *c = &sCgbChannels[1];
        if (nr24 & 0x80) {
            c->active   = 1;
            c->type     = 2;
            c->envVol   = (nr22 >> 4) & 0xF;
            c->envDir   = (nr22 & 0x08) ? 1 : -1;
            c->envSteps = nr22 & 0x07;
            c->envTimer = c->envSteps;
        }
        c->active = (c->active && c->envVol > 0);
    }

    /* Wave (CH3) */
    {
        u8 nr30 = _REG8(REG_OFFSET_SOUND3CNT_L);
        u8 nr34 = _REG8(REG_OFFSET_SOUND3CNT_X + 1);
        CgbChannel *c = &sCgbChannels[2];
        c->active = (nr30 & 0x80) != 0;
        if (nr34 & 0x80) {
            c->type  = 3;
            c->phase = 0;
            /* Copy wave RAM from register file */
            for (int i = 0; i < 16; i++)
                c->waveRAM[i] = _REG8(REG_OFFSET_SOUND3CNT_L + 0x30 + i);
        }
    }

    /* Noise (CH4) */
    {
        u8 nr42 = _REG8(REG_OFFSET_SOUND4CNT_L + 1);
        u8 nr44 = _REG8(REG_OFFSET_SOUND4CNT_H + 1);
        CgbChannel *c = &sCgbChannels[3];
        if (nr44 & 0x80) {
            c->active    = 1;
            c->type      = 4;
            c->envVol    = (nr42 >> 4) & 0xF;
            c->envDir    = (nr42 & 0x08) ? 1 : -1;
            c->envSteps  = nr42 & 0x07;
            c->lfsrState = 0x7FFF;
        }
    }
}

/* -----------------------------------------------------------------------
 * N64_MixAudioFrame — fill one stereo audio buffer
 *
 * Called each VBlank (or from AI interrupt) to prepare the next buffer.
 * --------------------------------------------------------------------- */
static void MixAudioFrame(s16 *buf, int samples)
{
    /* Zero the output buffer */
    memset(buf, 0, samples * 2 * sizeof(s16));

    struct SoundInfo *si = GetSoundInfo();
    if (!si) return;

    /* Read master volume from SOUNDCNT_H */
    u16 cntH = _REG16(REG_OFFSET_SOUNDCNT_H);
    int volShift = 2 - ((cntH >> 2) & 3);  /* 0=25%→>>2, 1=50%→>>1, 2=100%→>>0 */
    if (volShift < 0) volShift = 0;

    /* ------------------------------------------------------------------
     * Mix PCM channels
     * Each channel: 8-bit signed samples, resampled to N64_AUDIO_SAMPLE_RATE
     * ------------------------------------------------------------------ */
    M4AChannel *channels = si->chans;
    int numChannels = si->maxChans;
    if (numChannels > M4A_MAX_CHANNELS) numChannels = M4A_MAX_CHANNELS;

    for (int ch = 0; ch < numChannels; ch++) {
        M4AChannel *c = &channels[ch];
        if (!(c->statusFlags & M4A_STATUS_ACTIVE)) continue;
        if (!c->wav || c->frequency == 0)          continue;
        if (c->type & M4A_TYPE_CGB)                continue; /* handled below */

        /* Resample ratio: increment = srcFreq / dstFreq in fixed-point (16.16) */
        u32 rateInc = (u32)(((u64)c->frequency << 16) / N64_AUDIO_SAMPLE_RATE);
        u32 pos     = c->fw;   /* current sample position (16.16 fixed-point) */

        int volL = (c->leftVolume  * c->envelopeVolume) >> 8;
        int volR = (c->rightVolume * c->envelopeVolume) >> 8;
        if (volL < 0) volL = 0; if (volL > 255) volL = 255;
        if (volR < 0) volR = 0; if (volR > 255) volR = 255;

        for (int i = 0; i < samples; i++) {
            int sampleIdx = (int)(pos >> 16);
            /* Loop handling */
            if (sampleIdx >= (int)c->wav->size) {
                if (c->statusFlags & M4A_STATUS_LOOP) {
                    sampleIdx = (int)c->wav->loopStart +
                                (sampleIdx - (int)c->wav->size) % (int)(c->wav->size - c->wav->loopStart);
                } else {
                    /* Channel done */
                    c->statusFlags &= ~M4A_STATUS_ACTIVE;
                    break;
                }
            }

            /* 8-bit signed sample → 16-bit */
            s8 sample = c->wav->data[sampleIdx];
            s32 s16L  = (s32)sample * volL >> volShift;
            s32 s16R  = (s32)sample * volR >> volShift;

            /* Mix (clamp to s16 range) */
            s32 mixL = (s32)buf[i * 2 + 0] + (s16L << 6);
            s32 mixR = (s32)buf[i * 2 + 1] + (s16R << 6);
            if (mixL >  32767) mixL =  32767;
            if (mixL < -32768) mixL = -32768;
            if (mixR >  32767) mixR =  32767;
            if (mixR < -32768) mixR = -32768;
            buf[i * 2 + 0] = (s16)mixL;
            buf[i * 2 + 1] = (s16)mixR;

            pos += rateInc;
        }

        c->fw = pos;  /* save position for next frame */
    }

    /* ------------------------------------------------------------------
     * Mix CGB channels (square waves, noise)
     * ------------------------------------------------------------------ */
    UpdateCgbChannels();

    for (int ch = 0; ch < 4; ch++) {
        CgbChannel *c = &sCgbChannels[ch];
        if (!c->active) continue;

        /* Phase increment for this channel's frequency */
        if (c->freq > 0)
            c->phaseInc = (u32)(((u64)c->freq << 16) / N64_AUDIO_SAMPLE_RATE);

        for (int i = 0; i < samples; i++) {
            s16 sample = 0;

            if (c->type == 1 || c->type == 2) {
                /* Square wave */
                float duty  = sDutyCycle[(c->type == 1) ?
                    ((_REG8(REG_OFFSET_SOUND1CNT_H) >> 6) & 3) :
                    ((_REG8(REG_OFFSET_SOUND2CNT_L) >> 6) & 3)];
                float phase = (float)(c->phase >> 16) / 65536.0f;
                sample = (phase < duty) ? (s16)(c->envVol * 512) : (s16)(-c->envVol * 512);
            } else if (c->type == 3) {
                /* Wave channel: 32 4-bit samples in wave RAM */
                u32 wavePos = (c->phase >> 16) & 31;
                u8 nybble = (wavePos & 1)
                    ? (c->waveRAM[wavePos / 2] & 0x0F)
                    : (c->waveRAM[wavePos / 2] >> 4);
                sample = (s16)((int)nybble - 8) * 2048;
            } else if (c->type == 4) {
                /* Noise: LFSR */
                if (c->phase >> 16 != (c->phase - c->phaseInc) >> 16) {
                    int feedback = ((c->lfsrState ^ (c->lfsrState >> 1)) & 1);
                    c->lfsrState = (c->lfsrState >> 1) | (feedback << 14);
                }
                sample = (c->lfsrState & 1) ? (s16)(c->envVol * 512) : (s16)(-c->envVol * 512);
            }

            /* CGB channels are mono; apply to both channels at 25% master */
            s32 mixL = (s32)buf[i * 2 + 0] + (sample >> 2);
            s32 mixR = (s32)buf[i * 2 + 1] + (sample >> 2);
            if (mixL >  32767) mixL =  32767;
            if (mixL < -32768) mixL = -32768;
            if (mixR >  32767) mixR =  32767;
            if (mixR < -32768) mixR = -32768;
            buf[i * 2 + 0] = (s16)mixL;
            buf[i * 2 + 1] = (s16)mixR;

            c->phase += c->phaseInc;
        }
    }
}

/* -----------------------------------------------------------------------
 * N64_InitAI — configure the N64 Audio Interface
 * --------------------------------------------------------------------- */
void N64_InitAI(void)
{
    memset(sAudioBufs, 0, sizeof(sAudioBufs));
    sFillBuf = 0;
    sPlayBuf = 1;
    sAIBusy  = 0;

    /* AI DAC rate: NTSC clock / (rate + 1) = sample rate
     * NTSC AI clock = 48681812 Hz
     * For 32000 Hz: 48681812 / 32000 - 1 ≈ 1521 */
    u32 dacRate = (48681812 / N64_AUDIO_SAMPLE_RATE) - 1;
    AI_REG_WR(AI_DACRATE_REG, dacRate);
    AI_REG_WR(AI_BITRATE_REG, 15);       /* 16-bit */
    AI_REG_WR(AI_CONTROL_REG, 1);        /* DMA enable */

    /* Pre-fill first buffer (silence) */
    MixAudioFrame(sAudioBufs[sPlayBuf], N64_AUDIO_SAMPLES_PER_BUF);

    /* Start playing the first buffer */
    u32 physAddr = (u32)((uintptr_t)sAudioBufs[sPlayBuf] & 0x0FFFFFFF);
    AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
    AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);
    sAIBusy = 1;
}

/* -----------------------------------------------------------------------
 * N64_AudioRefill — called from interrupt.c when AI DMA buffer is empty
 * --------------------------------------------------------------------- */
void N64_AudioRefill(void)
{
    /* Queue the fill buffer for playback */
    u32 physAddr = (u32)((uintptr_t)sAudioBufs[sFillBuf] & 0x0FFFFFFF);
    AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
    AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);

    /* Swap buffers */
    int tmp  = sFillBuf;
    sFillBuf = sPlayBuf;
    sPlayBuf = tmp;
    sAIBusy  = 1;

    /* Mix the next frame into the newly free fill buffer */
    MixAudioFrame(sAudioBufs[sFillBuf], N64_AUDIO_SAMPLES_PER_BUF);
}

/* -----------------------------------------------------------------------
 * m4aSoundVSync — called each VBlank from main.c
 *
 * On GBA this updates the M4A timing counters and triggers the sound DMA.
 * On N64 the AI interrupt drives audio; we just kick off mixing if the AI
 * is idle (e.g. on the very first frame).
 * --------------------------------------------------------------------- */
void m4aSoundVSync(void)
{
    if (!sAIBusy) {
        MixAudioFrame(sAudioBufs[sFillBuf], N64_AUDIO_SAMPLES_PER_BUF);
        u32 physAddr = (u32)((uintptr_t)sAudioBufs[sFillBuf] & 0x0FFFFFFF);
        AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
        AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);
        sAIBusy = 1;
    }
}

/* -----------------------------------------------------------------------
 * m4aSoundVSyncOff — called during soft reset (DoSoftReset in main.c)
 * Stop audio DMA.
 * --------------------------------------------------------------------- */
void m4aSoundVSyncOff(void)
{
    AI_REG_WR(AI_CONTROL_REG, 0); /* disable DMA */
    sAIBusy = 0;
}

/* -----------------------------------------------------------------------
 * m4aSoundMain — called each VBlank from main.c's VBlankIntr
 *
 * On GBA this runs the full M4A mixer in IWRAM.  On N64 the mixing is done
 * asynchronously in N64_AudioRefill() driven by the AI interrupt.  We still
 * call the M4A sequencer to advance music playback state.
 * --------------------------------------------------------------------- */
void m4aSoundMain(void)
{
    /* The M4A sequencer (m4a.c) updates channel state.  The actual PCM
     * mixing is done in MixAudioFrame() called from N64_AudioRefill(). */
    /* m4a.c will call SoundMain() which we stub below. */
}

/* -----------------------------------------------------------------------
 * SoundMain / SoundMainRAM — the GBA ARM assembly mixer entry points
 *
 * On GBA, m4a.c calls SoundMain() (or SoundMainRAM() when running from
 * IWRAM).  On N64 these are no-ops because the mixer runs from
 * MixAudioFrame() in the AI interrupt.
 * --------------------------------------------------------------------- */
void SoundMain(void)    { /* no-op — mixing done in AI interrupt */ }
/* SoundMainRAM: on GBA this is ARM code that gets copied to IWRAM.
 * On N64 it's a dummy char array so m4a.c's memcpy in m4aSoundInit
 * compiles but copies harmless zeros. */
char SoundMainRAM[4] = {0};

/* -----------------------------------------------------------------------
 * M4A library internals — all from m4a_1.s (excluded from N64 build).
 * These are stub implementations; actual audio runs via N64_AudioRefill().
 * --------------------------------------------------------------------- */
void SoundMainBTM(void)   { /* no-op */ }
void TrackStop(struct MusicPlayerInfo *mpi, struct MusicPlayerTrack *trk)
    { (void)mpi; (void)trk; }
void MPlayMain(struct MusicPlayerInfo *mpi) { (void)mpi; }
void MPlayExtender(struct CgbChannel *cgb)  { (void)cgb; }
void FadeOutBody(struct MusicPlayerInfo *mpi) { (void)mpi; }
void RealClearChain(void *x)  { (void)x; }
void SampleFreqSet(u32 freq) { (void)freq; }
void TrkVolPitSet(struct MusicPlayerInfo *mpi, struct MusicPlayerTrack *trk)
    { (void)mpi; (void)trk; }

/* M4A MIDI command handlers — called via jump table in m4a_tables */
void ply_fine(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_goto(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_patt(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_pend(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_rept(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_prio(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_tempo(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_keysh(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_voice(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_vol(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)   { (void)m;(void)t; }
void ply_pan(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)   { (void)m;(void)t; }
void ply_bend(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_bendr(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_lfodl(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_lfos(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_mod(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)   { (void)m;(void)t; }
void ply_modt(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_tune(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_port(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)  { (void)m;(void)t; }
void ply_endtie(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t){ (void)m;(void)t; }
void ply_xxx(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t)   { (void)m;(void)t; }
void ply_xcmd_0D(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t){ (void)m;(void)t; }
void ply_xatta(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xdeca(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xsust(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xrele(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xiecv(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xiecl(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xleng(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xswee(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xtype(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xwave(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }
void ply_xwait(struct MusicPlayerInfo *m, struct MusicPlayerTrack *t) { (void)m;(void)t; }

/* gNumMusicPlayers — defines how many music players exist.
 * char[] so NUM_MUSIC_PLAYERS can cast it to u16. We have 4 players. */
char gNumMusicPlayers[2] = {4, 0};

/* MusicPlayerInfo instances — defined here since m4a_1.s is excluded */
struct MusicPlayerInfo gMPlayInfo_BGM;
struct MusicPlayerInfo gMPlayInfo_SE1;
struct MusicPlayerInfo gMPlayInfo_SE2;
struct MusicPlayerInfo gMPlayInfo_SE3;

/* gMaxLines — used by MPlayExtender for CGB channel limit; unused on N64 */
char gMaxLines[1] = {0};

/* umul3232H32 — multiply two 32-bit values and return the high 32 bits.
 * Used by MidiKeyToFreq for pitch calculation. */
u32 umul3232H32(u32 a, u32 b)
{
    return (u32)(((u64)a * (u64)b) >> 32);
}

/* ply_note — note-on handler; no-op on N64 (we don't play GBA music format) */
void ply_note(u32 note_cmd, struct MusicPlayerInfo *mpi, struct MusicPlayerTrack *trk)
{
    (void)note_cmd; (void)mpi; (void)trk;
}

/* Dummy song data — a 1-track song that ends immediately (FINE = 0xB1) */
static u8 sDummySongPart[1] = { 0xB1 }; /* FINE command */

/* mus_dummy / dummy_song_header — stub song headers referenced by song table.
 * The song table stores the ADDRESS of these as .4byte symbols.
 * The flexible array member 'part' must be last; we initialize via compound lit. */
const struct SongHeader mus_dummy = {
    .trackCount = 1, .blockCount = 0, .priority = 0, .reverb = 0,
    .tone = NULL, .part = { sDummySongPart }
};
const struct SongHeader dummy_song_header = {
    .trackCount = 1, .blockCount = 0, .priority = 0, .reverb = 0,
    .tone = NULL, .part = { sDummySongPart }
};

/* -----------------------------------------------------------------------
 * High-level m4a API stubs (from src/m4a.c, excluded on N64)
 * All audio is silent/no-op on N64 — we have no GBA sound hardware.
 * --------------------------------------------------------------------- */
struct SoundInfo gSoundInfo;

/* PokemonCrySong stubs — cry songs array used by pokemon sound effects */
struct PokemonCrySong gPokemonCrySongs[1];

void m4aSoundInit(void)                                                       {}
void m4aSoundVSyncOn(void)                                                    {}
void m4aSongNumStart(u16 n)                                                   { (void)n; }
void m4aSongNumStartOrChange(u16 n)                                           { (void)n; }
void m4aSongNumStop(u16 n)                                                    { (void)n; }
void m4aMPlayAllStop(void)                                                    {}
void m4aMPlayStop(struct MusicPlayerInfo *mpi)                                { (void)mpi; }
void m4aMPlayContinue(struct MusicPlayerInfo *mpi)                            { (void)mpi; }
void m4aMPlayFadeOut(struct MusicPlayerInfo *mpi, u16 speed)                  { (void)mpi; (void)speed; }
void m4aMPlayFadeOutTemporarily(struct MusicPlayerInfo *mpi, u16 speed)       { (void)mpi; (void)speed; }
void m4aMPlayFadeIn(struct MusicPlayerInfo *mpi, u16 speed)                   { (void)mpi; (void)speed; }
void m4aMPlayImmInit(struct MusicPlayerInfo *mpi)                             { (void)mpi; }
void m4aMPlayTempoControl(struct MusicPlayerInfo *mpi, u16 tempo)             { (void)mpi; (void)tempo; }
void m4aMPlayVolumeControl(struct MusicPlayerInfo *mpi, u16 bits, u16 vol)    { (void)mpi; (void)bits; (void)vol; }
void m4aMPlayPitchControl(struct MusicPlayerInfo *mpi, u16 bits, s16 pitch)   { (void)mpi; (void)bits; (void)pitch; }
void m4aMPlayPanpotControl(struct MusicPlayerInfo *mpi, u16 bits, s8 pan)     { (void)mpi; (void)bits; (void)pan; }

/* Pokemon cry control stubs */
struct MusicPlayerInfo *SetPokemonCryTone(struct ToneData *tone)              { (void)tone; return NULL; }
void SetPokemonCryVolume(u8 val)                                              { (void)val; }
void SetPokemonCryPanpot(s8 val)                                              { (void)val; }
void SetPokemonCryPitch(s16 val)                                              { (void)val; }
void SetPokemonCryLength(u16 val)                                             { (void)val; }
void SetPokemonCryRelease(u8 val)                                             { (void)val; }
void SetPokemonCryProgress(u32 val)                                           { (void)val; }
bool32 IsPokemonCryPlaying(struct MusicPlayerInfo *mpi)                       { (void)mpi; return FALSE; }
void SetPokemonCryChorus(s8 val)                                              { (void)val; }
void SetPokemonCryStereo(u32 val)                                             { (void)val; }
void SetPokemonCryPriority(u8 val)                                            { (void)val; }
