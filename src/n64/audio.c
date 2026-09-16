/*
 * src/n64/audio.c
 *
 * N64 port — Audio Interface driver and PCM/CGB mixer
 *
 * The GBA's sound hardware is two 8-bit DirectSound FIFOs drained by DMA at
 * whatever rate a timer is programmed for, plus the Game Boy's four-channel
 * PSG. The N64 has neither: the AI wants whole buffers of 16-bit stereo
 * frames in RDRAM and plays them back at a rate the DAC divider sets. So the
 * GBA mixer in src/m4a_1.s has no counterpart here, and this file replaces
 * it -- everything above it, the sequencer in src/n64/m4a_core.c and the
 * player in src/m4a.c, is the game's own code running unmodified.
 *
 * Division of labour, and it matters for timing:
 *
 *   SoundMain(), once per VBlank, in the VI interrupt
 *       Runs the sequencer, steps every channel's ADSR envelope, and leaves
 *       the finished per-channel volumes in envelopeVolumeRight/Left.
 *
 *   MixAudioFrame(), once per AI interrupt, here
 *       Walks those channels and resamples them into the next AI buffer.
 *
 * A buffer is 512 frames, which at 32 kHz is 16 ms -- one video frame. That
 * is deliberate: envelopes advance once per VBlank, so a buffer spanning
 * exactly one of them applies each envelope step to the audio it belongs to.
 * It also holds output latency to a frame, which matters because the
 * compositor runs at 10-15 fps and the two clocks are otherwise unrelated.
 *
 * On sample rates: MidiKeyToFreq() hands back a channel's playback rate in
 * Hz, and the GBA mixer turns that into a step per output sample with
 *
 *     step = frequency * divFreq / 2^23,  divFreq = (2^24 / outputRate + 1) / 2
 *
 * which is exact enough that a sample at its own rate steps by 1.0. The same
 * arithmetic works here with divFreq recomputed for 32 kHz, so pitch comes
 * out right without touching the engine's tables.
 *
 * Not carried over: the GBA mixer's reverb, which worked by feeding the PCM
 * ring buffer back into itself and has no meaning for a buffer this size.
 */

#include <string.h>
#include "global.h"
#include "m4a.h"
#include "gba/m4a_internal.h"
#include "n64/asm_defs.h"

/* -----------------------------------------------------------------------
 * N64 AI register access
 * --------------------------------------------------------------------- */
#define AI_REG_WR(off, val) N64_HW_WR(N64_AI_BASE_REG, (off), (val))
#define AI_REG_RD(off)      N64_HW_RD(N64_AI_BASE_REG, (off))

/* -----------------------------------------------------------------------
 * Buffering
 * --------------------------------------------------------------------- */
#define N64_AUDIO_SAMPLE_RATE     32000
#define N64_AUDIO_SAMPLES_PER_BUF 512
#define N64_AUDIO_BUF_BYTES       (N64_AUDIO_SAMPLES_PER_BUF * 2 * (int)sizeof(s16))
#define N64_AUDIO_NUM_BUFS        2

/* The AI reads these by DMA. They are written through KSEG1 so the data
 * cache stays out of it and no writeback is needed before the DMA starts. */
static s16 sAudioBufsStorage[N64_AUDIO_NUM_BUFS][N64_AUDIO_SAMPLES_PER_BUF * 2]
    __attribute__((aligned(16)));
static s16 *sAudioBufs[N64_AUDIO_NUM_BUFS];

/* Channels accumulate here at full precision and are clamped once on the way
 * out, instead of every channel clamping against the running total. */
static s32 sMixBuf[N64_AUDIO_SAMPLES_PER_BUF * 2];

static int sFillBuf = 0;
static int sPlayBuf = 1;
static volatile int sAIBusy = 0;

/* step = frequency * N64_DIV_FREQ / 2^23 source samples per output sample */
#define N64_DIV_FREQ  ((16777216 / N64_AUDIO_SAMPLE_RATE + 1) >> 1)
#define N64_FW_SHIFT  23
#define N64_FW_MASK   ((1u << N64_FW_SHIFT) - 1)

/* WaveData's header is byte-swapped to big-endian at build time by
 * tools/n64_wavedata_be.py, so the fields read as plain C. The loop flag is
 * the byte the GBA engine calls `flags`, which is the high half of status. */
#define WAVE_DATA_FLAG_LOOP 0xC0
#define WaveIsLooped(wav)   ((((wav)->status >> 8) & WAVE_DATA_FLAG_LOOP) != 0)

/* -----------------------------------------------------------------------
 * Compressed samples
 *
 * Cries ship as MP2K's DPCM: a 0x21-byte block holds one absolute sample
 * followed by 64 4-bit deltas indexing gDeltaEncodingTable, unpacking to 64
 * samples. A channel playing one keeps a sample index rather than a pointer,
 * and the decoded block is cached because playback walks it in order -- at
 * 13 kHz that is one decode every 4.8 ms.
 * --------------------------------------------------------------------- */
extern const s8 gDeltaEncodingTable[];

#define DPCM_BLOCK_BYTES   0x21
#define DPCM_BLOCK_SAMPLES 64

static s8 sDecodedBlock[DPCM_BLOCK_SAMPLES];
static const struct WaveData *sDecodedWave = NULL;
static u32 sDecodedBlockIndex = 0xFFFFFFFFu;

static void DecodeDpcmBlock(const struct WaveData *wav, u32 blockIndex)
{
    const u8 *src = (const u8 *)wav + 0x10 + blockIndex * DPCM_BLOCK_BYTES;
    s8 *dst = sDecodedBlock;

    /* Blocks are 0x21 bytes, so they are not word-aligned and the source is
     * in cartridge ROM. Pull the whole block through word reads once. */
    u8 block[DPCM_BLOCK_BYTES];
    {
        uintptr_t a = (uintptr_t)src;
        u32 w = N64_ReadRomWord((const void *)(a & ~(uintptr_t)3));
        for (unsigned i = 0; i < DPCM_BLOCK_BYTES; i++, a++)
        {
            if ((a & 3) == 0)
                w = N64_ReadRomWord((const void *)a);
            block[i] = (u8)(w >> (8 * (3 - (a & 3))));
        }
    }

    const u8 *p = block;
    s32 value = (s8)*p++;
    *dst++ = (s8)value;

    /* The first byte contributes only its low nibble; every byte after gives
     * high then low, which is the order the encoder wrote them. */
    u32 byte = *p++;
    value += gDeltaEncodingTable[byte & 0xF];
    *dst++ = (s8)value;

    for (int i = 0; i < (DPCM_BLOCK_SAMPLES - 2) / 2; i++)
    {
        byte = *p++;
        value += gDeltaEncodingTable[byte >> 4];
        *dst++ = (s8)value;
        value += gDeltaEncodingTable[byte & 0xF];
        *dst++ = (s8)value;
    }

    sDecodedWave = wav;
    sDecodedBlockIndex = blockIndex;
}

static inline s32 DpcmSample(const struct WaveData *wav, u32 index)
{
    u32 block = index >> 6;

    if (wav != sDecodedWave || block != sDecodedBlockIndex)
        DecodeDpcmBlock(wav, block);

    return sDecodedBlock[index & (DPCM_BLOCK_SAMPLES - 1)];
}

/* -----------------------------------------------------------------------
 * PCM channels
 * --------------------------------------------------------------------- */
static void MixPcmChannels(struct SoundInfo *soundInfo, int samples)
{
    /* A fixed-frequency voice plays one source sample per output sample at
     * the engine's own mixing rate, so it is resampled from there. */
    u32 fixedInc = (u32)soundInfo->pcmFreq * N64_DIV_FREQ;

    struct SoundChannel *chan = soundInfo->chans;
    u32 maxChans = soundInfo->maxChans;
    if (maxChans > MAX_DIRECTSOUND_CHANNELS)
        maxChans = MAX_DIRECTSOUND_CHANNELS;

    for (u32 c = 0; c < maxChans; c++, chan++)
    {
        u8 status = chan->statusFlags;

        if (!(status & SOUND_CHANNEL_SF_ON))
            continue;

        /* SoundMain has not opened this one yet, so it has no pointer. */
        if (status & SOUND_CHANNEL_SF_START)
            continue;

        u8 type = chan->type;
        if (type & TONEDATA_TYPE_CGB)
            continue;

        struct WaveData *wav = chan->wav;
        if (wav == NULL)
            continue;

        s32 volR = chan->envelopeVolumeRight;
        s32 volL = chan->envelopeVolumeLeft;

        u32 inc = (type & TONEDATA_TYPE_FIX)
                ? fixedInc
                : chan->frequency * (u32)N64_DIV_FREQ;

        u32 fw = chan->fw;
        s32 count = (s32)chan->count;
        int looped = (status & SOUND_CHANNEL_SF_LOOP) != 0;
        s32 loopStart = (s32)wav->loopStart;
        s32 loopLen = (s32)wav->size - loopStart;
        s32 *out = sMixBuf;

        if (type & TONEDATA_TYPE_CMP)
        {
            /* currentPointer holds a sample index for a compressed voice. */
            s32 index = (s32)(uintptr_t)chan->currentPointer;

            for (int i = 0; i < samples; i++, out += 2)
            {
                if (count <= 0)
                {
                    if (!looped || loopLen <= 0)
                    {
                        chan->statusFlags = 0;
                        break;
                    }
                    s32 over = (-count) % loopLen;
                    index = loopStart + over;
                    count = loopLen - over;
                }

                s32 s = DpcmSample(wav, (u32)index);
                out[0] += s * volR;
                out[1] += s * volL;

                fw += inc;
                u32 advance = fw >> N64_FW_SHIFT;
                if (advance)
                {
                    fw &= N64_FW_MASK;
                    index += (s32)advance;
                    count -= (s32)advance;
                }
            }

            chan->currentPointer = (s8 *)(uintptr_t)index;
        }
        else
        {
            const s8 *p = chan->currentPointer;

            /* Samples live in cartridge ROM, where only word reads return
             * the right data. Playback walks them in order and a step is
             * usually well under one sample, so holding the containing word
             * serves several output frames per read -- correct on the PI bus
             * and fewer loads than a byte fetch would be anyway. */
            uintptr_t wordAddr = ~(uintptr_t)0;
            u32 word = 0;

            for (int i = 0; i < samples; i++, out += 2)
            {
                if (count <= 0)
                {
                    if (!looped || loopLen <= 0)
                    {
                        chan->statusFlags = 0;
                        break;
                    }
                    s32 over = (-count) % loopLen;
                    p = wav->data + loopStart + over;
                    count = loopLen - over;
                }

                uintptr_t a = (uintptr_t)p;
                if ((a & ~(uintptr_t)3) != wordAddr)
                {
                    wordAddr = a & ~(uintptr_t)3;
                    word = N64_ReadRomWord((const void *)wordAddr);
                }
                s32 s = (s8)(word >> (8 * (3 - (a & 3))));

                out[0] += s * volR;
                out[1] += s * volL;

                fw += inc;
                u32 advance = fw >> N64_FW_SHIFT;
                if (advance)
                {
                    fw &= N64_FW_MASK;
                    p += advance;
                    count -= (s32)advance;
                }
            }

            chan->currentPointer = (s8 *)p;
        }

        chan->fw = fw;
        chan->count = (u32)count;
    }
}

/* -----------------------------------------------------------------------
 * CGB channels
 *
 * src/m4a.c's CgbSound() emulates the Game Boy envelope, sweep and length
 * counters in software and leaves the result in the CgbChannel structs, so
 * what is left here is the oscillators: two squares, a 32-step wave, and an
 * LFSR noise generator.
 *
 * Phase is 16.16 and the increment is computed once per buffer, which keeps
 * the divides out of the sample loop.
 * --------------------------------------------------------------------- */
/* A CGB oscillator swings +/-8 at volume 15, so it is scaled up to sit
 * alongside PCM, which reaches about +/-32,600 for one channel at full
 * volume. Seven bits puts a single PSG channel at roughly half that, which
 * is about where the GBA's mixer leaves it. */
#define CGB_GAIN_SHIFT 7

#define CGB_PHASE_BITS 16
#define CGB_PHASE_ONE  (1u << CGB_PHASE_BITS)

static u32 sCgbPhase[4];
static u16 sCgbLfsr = 0x7FFF;

/* Eighths of a period for which a square is high, by duty code. */
static const u8 sDutyEighths[4] = {1, 2, 4, 6};

/* NR43's divisor codes, doubled so code 0 -- which means a half -- fits. */
static const u8 sNoiseDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};

static void MixCgbChannels(struct SoundInfo *soundInfo, int samples)
{
    struct CgbChannel *cgbChans = soundInfo->cgbChans;

    if (cgbChans == NULL)
        return;

    for (int ch = 0; ch < 4; ch++)
    {
        struct CgbChannel *chan = &cgbChans[ch];

        if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            continue;

        u32 volume = chan->envelopeVolume;   /* 0-15 */
        if (volume == 0)
            continue;

        /* pan holds this channel's NR51 enable bits, already masked to its
         * own pair: low nibble right, high nibble left. */
        u32 pan = chan->pan;
        s32 gainR = (pan & 0x0F) ? (s32)volume : 0;
        s32 gainL = (pan & 0xF0) ? (s32)volume : 0;
        if ((gainR | gainL) == 0)
            continue;

        gainR <<= CGB_GAIN_SHIFT;
        gainL <<= CGB_GAIN_SHIFT;

        u32 phase = sCgbPhase[ch];
        u32 phaseInc;
        s32 *out = sMixBuf;
        u32 type = chan->type;

        if (type == 4)
        {
            /* NR43: [shift:4][width:1][divisor:3]. The LFSR clocks at
             * 524288 / divisor / 2^(shift+1) Hz. */
            u32 nr43 = chan->frequency & 0xFF;
            u32 divisor = sNoiseDivisors[nr43 & 7];
            u32 shift = (nr43 >> 4) & 0xF;
            u32 rate = (524288u * 2u / divisor) >> (shift + 1);

            if (rate == 0)
                continue;

            phaseInc = (u32)(((u64)rate << CGB_PHASE_BITS) / N64_AUDIO_SAMPLE_RATE);
            int narrow = (nr43 & 8) != 0;

            for (int i = 0; i < samples; i++, out += 2)
            {
                s32 s = (sCgbLfsr & 1) ? -8 : 7;
                out[0] += s * gainR;
                out[1] += s * gainL;

                phase += phaseInc;
                while (phase >= CGB_PHASE_ONE)
                {
                    phase -= CGB_PHASE_ONE;
                    u32 feedback = (sCgbLfsr ^ (sCgbLfsr >> 1)) & 1;
                    sCgbLfsr >>= 1;
                    sCgbLfsr |= feedback << 14;
                    if (narrow)
                        sCgbLfsr = (u16)((sCgbLfsr & ~0x40u) | (feedback << 6));
                }
            }
        }
        else
        {
            /* Square and wave share the period formula and differ only in
             * the clock: 131072 Hz for a square, half that for the wave,
             * which reads 32 steps per period rather than 8. */
            u32 freqReg = chan->frequency & 0x7FF;
            u32 period = 2048 - freqReg;
            if (period == 0)
                continue;

            u32 hz = (type == 3 ? 65536u : 131072u) / period;
            phaseInc = (u32)(((u64)hz << CGB_PHASE_BITS) / N64_AUDIO_SAMPLE_RATE);

            if (type == 3)
            {
                /* Wave RAM is 16 bytes holding 32 nibbles, high nibble
                 * first. NR32 scales them: mute, full, half, quarter. */
                const u8 *wave = (const u8 *)chan->currentPointer;
                if (wave == NULL)
                    wave = (const u8 *)chan->wavePointer;
                if (wave == NULL)
                    continue;

                u32 level = (_REG8(REG_OFFSET_SOUND3CNT_H + 1) >> 5) & 3;
                if (level == 0)
                    continue;
                u32 shift = level - 1;   /* 0 = full, 1 = half, 2 = quarter */

                for (int i = 0; i < samples; i++, out += 2)
                {
                    u32 step = (phase >> CGB_PHASE_BITS) & 31;
                    u32 byte = wave[step >> 1];
                    s32 nibble = (step & 1) ? (s32)(byte & 0xF) : (s32)(byte >> 4);
                    s32 s = (nibble - 8) >> shift;

                    out[0] += s * gainR;
                    out[1] += s * gainL;
                    phase += phaseInc;
                }
            }
            else
            {
                u32 duty = ((u32)(uintptr_t)chan->wavePointer) & 3;
                u32 high = sDutyEighths[duty];

                for (int i = 0; i < samples; i++, out += 2)
                {
                    u32 eighth = (phase >> (CGB_PHASE_BITS - 3)) & 7;
                    s32 s = (eighth < high) ? 7 : -8;

                    out[0] += s * gainR;
                    out[1] += s * gainL;
                    phase += phaseInc;
                }
            }
        }

        sCgbPhase[ch] = phase;
    }
}

/* -----------------------------------------------------------------------
 * MixAudioFrame — fill one AI buffer
 * --------------------------------------------------------------------- */
static void MixAudioFrame(s16 *out, int samples)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    memset(sMixBuf, 0, (size_t)samples * 2 * sizeof(s32));

    if (soundInfo != NULL)
    {
        MixPcmChannels(soundInfo, samples);
        MixCgbChannels(soundInfo, samples);
    }

    for (int i = 0; i < samples * 2; i++)
    {
        s32 v = sMixBuf[i];
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        out[i] = (s16)v;
    }
}

/* -----------------------------------------------------------------------
 * N64_InitAI — configure the N64 Audio Interface
 * --------------------------------------------------------------------- */
void N64_InitAI(void)
{
    for (int i = 0; i < N64_AUDIO_NUM_BUFS; i++)
        sAudioBufs[i] = (s16 *)((uintptr_t)sAudioBufsStorage[i] | 0x20000000u);

    memset(sAudioBufsStorage, 0, sizeof(sAudioBufsStorage));
    sFillBuf = 0;
    sPlayBuf = 1;
    sAIBusy  = 0;

    /* AI DAC rate: NTSC clock / (rate + 1) = sample rate. The NTSC AI clock
     * is 48681812 Hz, so 32 kHz wants a divider of 1520. */
    u32 dacRate = (48681812 / N64_AUDIO_SAMPLE_RATE) - 1;
    AI_REG_WR(AI_DACRATE_REG, dacRate);
    AI_REG_WR(AI_BITRATE_REG, 15);       /* 16-bit */
    AI_REG_WR(AI_CONTROL_REG, 1);        /* DMA enable */

    MixAudioFrame(sAudioBufs[sPlayBuf], N64_AUDIO_SAMPLES_PER_BUF);

    u32 physAddr = (u32)((uintptr_t)sAudioBufsStorage[sPlayBuf] & 0x0FFFFFFF);
    AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
    AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);
    sAIBusy = 1;
}

/* -----------------------------------------------------------------------
 * N64_AudioRefill — the AI drained a buffer; queue the next and fill one
 * --------------------------------------------------------------------- */
void N64_AudioRefill(void)
{
    u32 physAddr = (u32)((uintptr_t)sAudioBufsStorage[sFillBuf] & 0x0FFFFFFF);
    AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
    AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);

    int tmp  = sFillBuf;
    sFillBuf = sPlayBuf;
    sPlayBuf = tmp;
    sAIBusy  = 1;
    MixAudioFrame(sAudioBufs[sFillBuf], N64_AUDIO_SAMPLES_PER_BUF);
}

/* -----------------------------------------------------------------------
 * m4aSoundVSync — called each VBlank
 *
 * On the GBA this reloads the PCM DMA. Here the AI interrupt drives
 * everything, so this only restarts the chain if it ever stops -- which it
 * does exactly once, before the first AI interrupt arrives.
 * --------------------------------------------------------------------- */
void m4aSoundVSync(void)
{
    if (!sAIBusy)
    {
        MixAudioFrame(sAudioBufs[sFillBuf], N64_AUDIO_SAMPLES_PER_BUF);
        u32 physAddr = (u32)((uintptr_t)sAudioBufsStorage[sFillBuf] & 0x0FFFFFFF);
        AI_REG_WR(AI_DRAM_ADDR_REG, physAddr);
        AI_REG_WR(AI_LEN_REG,       N64_AUDIO_BUF_BYTES);
        sAIBusy = 1;
    }
}

/* -----------------------------------------------------------------------
 * Pieces of the GBA engine with nothing to do here
 *
 * SoundMainRAM is the ARM mixer the GBA copies into IWRAM and jumps to --
 * the code this file replaces. m4aSoundInit() still copies a buffer's worth
 * of bytes out of it, so it has to be that big even though all of them are
 * zero and nothing ever jumps there.
 * --------------------------------------------------------------------- */
char SoundMainRAM[0x800] = {0};

/* The one entry in gSongTable with no MIDI behind it. The table stores its
 * address, so it has to exist; a single FINE command is a song that ends the
 * instant it starts. */
static u8 sDummySongPart[1] = { 0xB1 };

const struct SongHeader dummy_song_header = {
    .trackCount = 1, .blockCount = 0, .priority = 0, .reverb = 0,
    .tone = NULL, .part = { sDummySongPart }
};
