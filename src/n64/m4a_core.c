/*
 * src/n64/m4a_core.c
 *
 * N64 port — the M4A sequencer, ported from src/m4a_1.s
 *
 * pokeemerald's sound engine comes in two halves. src/m4a.c is portable C:
 * it opens music players, starts and stops songs, computes per-track volume
 * and pitch, and drives the CGB channels. src/m4a_1.s is hand-written ARM
 * that does everything else -- the per-frame command interpreter, the note
 * allocator, the envelope stepping, and the PCM mixer -- and it is the
 * reason the port was silent: with it excluded and stubbed out, nothing
 * ever started a channel.
 *
 * This file is that assembly in C, minus the mixer. The mixer is a
 * different problem on this machine: the GBA one writes 8-bit samples into
 * a FIFO the DMA drains at whatever rate a timer is programmed for, while
 * the N64 wants 16-bit stereo frames handed to the AI. src/n64/audio.c does
 * that part and reads the channel state this file maintains.
 *
 * What is faithful here, because songs depend on all of it:
 *   - the running-status command stream, including the pattern stack and
 *     the repeat counter
 *   - note allocation, which steals the lowest-priority channel and breaks
 *     ties on track address exactly the way the original does
 *   - key-split and rhythm voices, which reach through the track's tone
 *     into a sub-voicegroup
 *   - the ADSR state machine with its pseudo-echo tail
 *   - the LFO, whose phase and depth feed back into volume or pitch
 *
 * Two things the original does that are meaningless here are dropped: the
 * jump-table address check (it guards against reading the GBA BIOS ROM) and
 * the mixer's reverb, which operated on the GBA's PCM ring buffer.
 */

#include "global.h"
#include "gba/m4a_internal.h"

extern const u8 gClockTable[];

/* WaveData's header is byte-swapped to big-endian at build time by
 * tools/n64_wavedata_be.py, so its fields read as plain C. The loop flag is
 * the byte the GBA engine calls `flags`, which is the high half of status. */
#define WAVE_DATA_FLAG_LOOP 0xC0
#define WaveIsLooped(wav)   ((((wav)->status >> 8) & WAVE_DATA_FLAG_LOOP) != 0)
extern void *const gMPlayJumpTableTemplate[];

/* The jump table is declared as unprototyped function pointers so that the
 * same array can hold handlers with different signatures. Track commands all
 * take (mplayInfo, track). */
typedef void (*TrackCmdFunc)(struct MusicPlayerInfo *, struct MusicPlayerTrack *);

#define MPLAY_JUMP_TABLE_SIZE 36

/* -----------------------------------------------------------------------
 * Small helpers the assembly had as shared tails
 * --------------------------------------------------------------------- */

u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((u64)multiplier * (u64)multiplicand) >> 32);
}

/* Read one byte of the command stream and step past it. */
static inline u8 ReadTrackByte(struct MusicPlayerTrack *track)
{
    u8 value = *track->cmdPtr;
    track->cmdPtr++;
    return value;
}

/* A GOTO or PATT target is a 4-byte address sitting in the command stream at
 * whatever offset the stream reached, so it is read a byte at a time rather
 * than as a word. The assembler wrote it big-endian, this being a big-endian
 * build; the GBA original reads the same four bytes little-endian. */
static inline u8 *ReadTrackPointer(const u8 *p)
{
    return (u8 *)(uintptr_t)(((u32)p[0] << 24) | ((u32)p[1] << 16)
                           | ((u32)p[2] << 8)  | (u32)p[3]);
}

/* -----------------------------------------------------------------------
 * Clear64byte / ClearChain, reached through the jump table
 * --------------------------------------------------------------------- */

void SoundMainBTM(void *p)
{
    u32 *dst = (u32 *)p;
    for (int i = 0; i < 16; i++)
        dst[i] = 0;
}

void RealClearChain(void *x)
{
    struct SoundChannel *chan = (struct SoundChannel *)x;
    struct MusicPlayerTrack *track = chan->track;

    if (track == NULL)
        return;

    struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
    struct SoundChannel *prev = (struct SoundChannel *)chan->prevChannelPointer;

    if (prev != NULL)
        prev->nextChannelPointer = next;
    else
        track->chan = next;

    if (next != NULL)
        next->prevChannelPointer = prev;

    chan->track = NULL;
}

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    for (int i = 0; i < MPLAY_JUMP_TABLE_SIZE; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

/* -----------------------------------------------------------------------
 * Track commands
 * --------------------------------------------------------------------- */

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan = track->chan;

    while (chan != NULL)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;

        RealClearChain(chan);
        chan = (struct SoundChannel *)chan->nextChannelPointer;
    }

    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->cmdPtr = ReadTrackPointer(track->cmdPtr);
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 level = track->patternLevel;

    if (level >= 3)
    {
        /* Nesting deeper than the stack can hold ends the track, which is
         * what the original does rather than overrun it. */
        ply_fine(mplayInfo, track);
        return;
    }

    track->patternStack[level] = track->cmdPtr + 4;
    track->patternLevel = level + 1;
    ply_goto(mplayInfo, track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel != 0)
    {
        u32 level = track->patternLevel - 1;
        track->patternLevel = level;
        track->cmdPtr = track->patternStack[level];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    /* A repeat count of zero means "forever". */
    if (*track->cmdPtr == 0)
    {
        track->cmdPtr++;
        ply_goto(mplayInfo, track);
        return;
    }

    u8 taken = track->repN + 1;
    track->repN = taken;

    u8 total = ReadTrackByte(track);

    if (taken < total)
    {
        ply_goto(mplayInfo, track);
        return;
    }

    track->repN = 0;
    track->cmdPtr += 4;   /* step over the target, the count is already past */
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->priority = ReadTrackByte(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u16 tempo = (u16)(ReadTrackByte(track) * 2);
    mplayInfo->tempoD = tempo;
    mplayInfo->tempoI = (u16)((tempo * mplayInfo->tempoU) >> 8);
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->keyShift = (s8)ReadTrackByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 voice = ReadTrackByte(track);

    /* Voicegroups live in cartridge ROM, which only answers word reads, so
     * the tone is copied out three words at a time rather than field by
     * field. Every entry is 12 bytes from a 4-aligned base, so this is
     * always aligned. */
    N64_ReadRomAligned(&track->tone, &mplayInfo->tone[voice],
                       sizeof(struct ToneData));
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->vol = ReadTrackByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->pan = (s8)(ReadTrackByte(track) - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bend = (s8)(ReadTrackByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bendRange = ReadTrackByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoDelay = ReadTrackByte(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 type = ReadTrackByte(track);

    if (track->modT != type)
    {
        track->modT = type;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->tune = (s8)(ReadTrackByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    /* Writes straight into the CGB sound registers at an offset the song
     * supplies. On this machine those are the software register file that
     * audio.c reads back. */
    u32 reg = ReadTrackByte(track);
    u8 value = ReadTrackByte(track);

    _REG8(REG_OFFSET_SOUND1CNT_L + reg) = value;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoSpeed = ReadTrackByte(track);

    if (track->lfoSpeed == 0)
        ClearModM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->mod = ReadTrackByte(track);

    if (track->mod == 0)
        ClearModM(track);
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 key = *track->cmdPtr;

    if (key < 0x80)
    {
        track->key = key;
        track->cmdPtr++;
    }
    else
    {
        key = track->key;
    }

    /* Release the first still-playing channel holding this key. */
    for (struct SoundChannel *chan = track->chan; chan != NULL;
         chan = (struct SoundChannel *)chan->nextChannelPointer)
    {
        if (!(chan->statusFlags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)))
            continue;
        if (chan->statusFlags & SOUND_CHANNEL_SF_STOP)
            continue;
        if (chan->midiKey != key)
            continue;

        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        return;
    }
}

/* -----------------------------------------------------------------------
 * Channel volume
 *
 * rhythmPan biases the track's own panning for a rhythm voice; velocity and
 * the track volume the caller already folded into volMR/volML do the rest.
 * --------------------------------------------------------------------- */
static void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    s32 velocity = chan->velocity;
    s32 pan = (s8)chan->rhythmPan;

    s32 right = ((0x80 + pan) * velocity * track->volMR) >> 14;
    if (right > 0xFF)
        right = 0xFF;
    chan->rightVolume = (u8)right;

    s32 left = ((0x7F - pan) * velocity * track->volML) >> 14;
    if (left > 0xFF)
        left = 0xFF;
    chan->leftVolume = (u8)left;
}

/* -----------------------------------------------------------------------
 * ply_note — start a note
 * --------------------------------------------------------------------- */
void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct SoundChannel *chan;
    struct ToneData *tone = &track->tone;
    struct ToneData subTone;
    s32 rhythmPan = 0;
    u32 key;
    u32 priority;
    u32 cgbType;

    track->gateTime = gClockTable[note_cmd];

    /* key, velocity and a gate-time extension, each optional and each
     * recognised by being below 0x80. */
    {
        u8 *p = track->cmdPtr;
        if (*p < 0x80)
        {
            track->key = *p++;
            if (*p < 0x80)
            {
                track->velocity = *p++;
                if (*p < 0x80)
                    track->gateTime += *p++;
            }
            track->cmdPtr = p;
        }
    }

    u8 trackToneType = tone->type;

    if (trackToneType & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        /* Both kinds reach into a sub-voicegroup. A key split looks the note
         * up in a table first; a rhythm voice indexes by the note itself. */
        u32 instrument;

        if (trackToneType & TONEDATA_TYPE_SPL)
        {
            /* For a split voice the last word of the tone is the table, not
             * the envelope. It sits in cartridge ROM beside the voicegroups,
             * so the byte comes out of its containing word. */
            const u8 *keySplitTable = *(const u8 **)&tone->attack;
            instrument = N64_ReadRomByte(keySplitTable + track->key);
        }
        else
        {
            instrument = track->key;
        }

        /* The sub-voicegroup is in cartridge ROM; take a word-read copy
         * before looking at any of its bytes. */
        N64_ReadRomAligned(&subTone,
                           (u8 *)tone->wav + instrument * sizeof(struct ToneData),
                           sizeof(struct ToneData));
        tone = &subTone;

        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
            return;   /* a split inside a split is not a thing */

        if (trackToneType & TONEDATA_TYPE_RHY)
        {
            if (tone->pan_sweep & 0x80)
                rhythmPan = (s8)((tone->pan_sweep - TONEDATA_P_S_PAN) * 2);
            key = tone->key;
        }
        else
        {
            key = track->key;
        }
    }
    else
    {
        key = track->key;
    }

    priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    cgbType = tone->type & TONEDATA_TYPE_CGB;

    if (cgbType)
    {
        /* A CGB voice owns exactly one hardware channel, so the only
         * question is whether this note outranks whatever is on it. */
        struct CgbChannel *cgbChans = soundInfo->cgbChans;

        if (cgbChans == NULL)
            return;

        struct CgbChannel *cgbChan = &cgbChans[cgbType - 1];

        if ((cgbChan->statusFlags & SOUND_CHANNEL_SF_ON)
            && !(cgbChan->statusFlags & SOUND_CHANNEL_SF_STOP))
        {
            if (cgbChan->priority > priority)
                return;
            if (cgbChan->priority == priority
                && (uintptr_t)cgbChan->track < (uintptr_t)track)
                return;
        }

        chan = (struct SoundChannel *)cgbChan;
    }
    else
    {
        /* Pick a PCM channel: a free one if there is one, otherwise the
         * weakest. Channels already releasing are fair game before any
         * channel that is still held, and among equals the one belonging to
         * the highest track address loses -- that tie-break is what keeps
         * two tracks from fighting over the same channel every frame. */
        struct SoundChannel *best = NULL;
        u32 bestPriority = priority;
        struct MusicPlayerTrack *bestTrack = track;
        int foundReleasing = 0;
        u32 maxChans = soundInfo->maxChans;

        chan = soundInfo->chans;

        for (u32 i = 0; i < maxChans; i++, chan++)
        {
            u8 status = chan->statusFlags;

            if (!(status & SOUND_CHANNEL_SF_ON))
            {
                best = chan;
                goto gotChannel;
            }

            if (status & SOUND_CHANNEL_SF_STOP)
            {
                if (!foundReleasing)
                {
                    foundReleasing = 1;
                    bestPriority = chan->priority;
                    bestTrack = chan->track;
                    best = chan;
                    continue;
                }
            }
            else if (foundReleasing)
            {
                continue;
            }

            if (chan->priority < bestPriority)
            {
                bestPriority = chan->priority;
                bestTrack = chan->track;
                best = chan;
            }
            else if (chan->priority == bestPriority)
            {
                if ((uintptr_t)chan->track > (uintptr_t)bestTrack)
                {
                    bestTrack = chan->track;
                    best = chan;
                }
                else if ((uintptr_t)chan->track == (uintptr_t)bestTrack)
                {
                    best = chan;
                }
            }
        }

        if (best == NULL)
            return;

        chan = best;
    }

gotChannel:
    /* Unhook it from whatever track had it and put it at the head of ours. */
    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        ((struct SoundChannel *)track->chan)->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModM(track);

    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey  = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)priority;
    chan->key      = (u8)key;
    chan->rhythmPan = (u8)rhythmPan;
    chan->type     = tone->type;
    chan->wav      = tone->wav;
    chan->attack   = tone->attack;
    chan->decay    = tone->decay;
    chan->sustain  = tone->sustain;
    chan->release  = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;

    ChnVolSetAsm(chan, track);

    s32 finalKey = (s32)chan->key + (s8)track->keyM;
    if (finalKey < 0)
        finalKey = 0;

    if (cgbType)
    {
        struct CgbChannel *cgbChan = (struct CgbChannel *)chan;
        u8 sweep = tone->pan_sweep;

        cgbChan->length = tone->length;

        /* Bit 7 set means the byte is panning, not a sweep, and a sweep of
         * zero in the low bits is no sweep either; both mean "off", which
         * the hardware spells 8. */
        if ((sweep & 0x80) || !(sweep & 0x70))
            sweep = 8;
        cgbChan->sweep = sweep;

        chan->frequency = soundInfo->MidiKeyToCgbFreq(cgbType, (u8)finalKey, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(chan->wav, (u8)finalKey, track->pitM);
    }

    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

/* -----------------------------------------------------------------------
 * TrackStop — silence a track's channels outright, no release
 * --------------------------------------------------------------------- */
void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (!(track->flags & MPT_FLG_EXIST))
        return;

    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    for (struct SoundChannel *chan = track->chan; chan != NULL;
         chan = (struct SoundChannel *)chan->nextChannelPointer)
    {
        if (chan->statusFlags != 0)
        {
            u8 cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (cgbType)
                soundInfo->CgbOscOff(cgbType);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }

    track->chan = NULL;
}

/* -----------------------------------------------------------------------
 * MPlayMain — one player's worth of a frame
 * --------------------------------------------------------------------- */
void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    if (mplayInfo->ident != ID_NUMBER)
        return;

    mplayInfo->ident++;

    /* Players are a linked list and each one runs the next; the head is what
     * SoundMain calls. */
    if (mplayInfo->MPlayMainNext != NULL)
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    if ((s32)mplayInfo->status < 0)     /* paused */
        goto done;

    FadeOutBody(mplayInfo);

    if ((s32)mplayInfo->status < 0)     /* the fade finished and paused us */
        goto done;

    /* tempoI is added every frame and a tick is spent for each whole 150
     * that accumulates, so a fast tempo runs several ticks per frame. */
    u32 tempoC = (u16)(mplayInfo->tempoC + mplayInfo->tempoI);

    for (;;)
    {
        mplayInfo->tempoC = (u16)tempoC;

        if (tempoC < 150)
            break;

        u32 trackBits = 0;
        u32 trackBit = 1;
        struct MusicPlayerTrack *track = mplayInfo->tracks;

        for (u32 i = mplayInfo->trackCount; i > 0; i--, track++, trackBit <<= 1)
        {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;

            trackBits |= trackBit;

            /* Count down each held note; when its gate time runs out the
             * channel moves into release. */
            {
                struct SoundChannel *chan = track->chan;
                while (chan != NULL)
                {
                    if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
                    {
                        if (chan->gateTime != 0 && --chan->gateTime == 0)
                            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                    }
                    else
                    {
                        ClearChain(chan);
                    }
                    chan = (struct SoundChannel *)chan->nextChannelPointer;
                }
            }

            if (track->flags & MPT_FLG_START)
            {
                /* Only the first 64 bytes: cmdPtr and the pattern stack live
                 * past that and MPlayStart has already set them. */
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 64;
                track->lfoSpeed = 22;
                track->tone.type = 1;
            }

            while (track->wait == 0)
            {
                u32 cmd = *track->cmdPtr;

                if (cmd >= 0x80)
                {
                    track->cmdPtr++;
                    if (cmd >= 0xBD)
                        track->runningStatus = (u8)cmd;
                }
                else
                {
                    cmd = track->runningStatus;
                }

                if (cmd >= 0xCF)
                {
                    soundInfo->plynote(cmd - 0xCF, mplayInfo, track);
                }
                else if (cmd > 0xB0)
                {
                    mplayInfo->cmd = (u8)(cmd - 0xB1);
                    TrackCmdFunc handler =
                        (TrackCmdFunc)soundInfo->MPlayJumpTable[cmd - 0xB1];
                    handler(mplayInfo, track);

                    if (track->flags == 0)
                        goto nextTrack;   /* the track ended */
                }
                else
                {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }

            track->wait--;

            /* LFO: a triangle that runs once the delay has elapsed, applied
             * to pitch or volume depending on modT. */
            if (track->lfoSpeed != 0 && track->mod != 0)
            {
                if (track->lfoDelayC != 0)
                {
                    track->lfoDelayC--;
                }
                else
                {
                    u32 phase = track->lfoSpeedC + track->lfoSpeed;
                    track->lfoSpeedC = (u8)phase;

                    s32 tri;
                    if ((s8)(u8)(phase - 0x40) < 0)
                        tri = (s8)(u8)phase;
                    else
                        tri = 0x80 - (s32)phase;

                    s32 depth = (track->mod * tri) >> 6;

                    if (((track->modM ^ depth) & 0xFF) != 0)
                    {
                        track->modM = (s8)depth;
                        track->flags |= track->modT == 0 ? MPT_FLG_PITCHG
                                                         : MPT_FLG_VOLCHG;
                    }
                }
            }

        nextTrack:
            ;
        }

        mplayInfo->clock++;

        if (trackBits == 0)
        {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }

        mplayInfo->status = trackBits;
        tempoC = (u16)(mplayInfo->tempoC - 150);
    }

    /* Push whatever moved this frame out to the channels. */
    {
        struct MusicPlayerTrack *track = mplayInfo->tracks;

        for (u32 i = mplayInfo->trackCount; i > 0; i--, track++)
        {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;
            if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
                continue;

            TrkVolPitSet(mplayInfo, track);

            for (struct SoundChannel *chan = track->chan; chan != NULL;
                 chan = (struct SoundChannel *)chan->nextChannelPointer)
            {
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
                {
                    ClearChain(chan);
                    continue;
                }

                u8 cgbType = chan->type & TONEDATA_TYPE_CGB;

                if (track->flags & MPT_FLG_VOLCHG)
                {
                    ChnVolSetAsm(chan, track);
                    if (cgbType)
                        ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
                }

                if (track->flags & MPT_FLG_PITCHG)
                {
                    s32 key = (s32)chan->key + (s8)track->keyM;
                    if (key < 0)
                        key = 0;

                    if (cgbType)
                    {
                        chan->frequency =
                            soundInfo->MidiKeyToCgbFreq(cgbType, (u8)key, track->pitM);
                        ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_PIT;
                    }
                    else
                    {
                        chan->frequency = MidiKeyToFreq(chan->wav, (u8)key, track->pitM);
                    }
                }
            }

            track->flags &= 0xF0;
        }
    }

done:
    mplayInfo->ident = ID_NUMBER;
}

/* -----------------------------------------------------------------------
 * SoundMain — the per-frame entry point, called from VBlank
 *
 * On the GBA this also mixed a frame of PCM. Here it runs the sequencer and
 * steps every channel's envelope, and src/n64/audio.c mixes from the AI
 * interrupt using the result.
 * --------------------------------------------------------------------- */
void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    if (soundInfo == NULL || soundInfo->ident != ID_NUMBER)
        return;

    soundInfo->ident++;

    if (soundInfo->MPlayMainHead != NULL)
        soundInfo->MPlayMainHead(soundInfo->musicPlayerHead);

    soundInfo->CgbSound();

    u32 masterVolume = soundInfo->masterVolume;
    struct SoundChannel *chan = soundInfo->chans;

    for (u32 i = soundInfo->maxChans; i > 0; i--, chan++)
    {
        u8 status = chan->statusFlags;
        u32 envelopeVolume;

        if (!(status & SOUND_CHANNEL_SF_ON))
            continue;

        if (status & SOUND_CHANNEL_SF_START)
        {
            /* A note that was stopped before it ever sounded just dies. */
            if (status & SOUND_CHANNEL_SF_STOP)
            {
                chan->statusFlags = 0;
                continue;
            }

            struct WaveData *wav = chan->wav;

            status = SOUND_CHANNEL_SF_ENV_ATTACK;
            chan->statusFlags = status;
            chan->currentPointer = wav->data + chan->count;
            chan->count = wav->size - chan->count;
            chan->fw = 0;
            envelopeVolume = 0;

            if (WaveIsLooped(wav))
            {
                status |= SOUND_CHANNEL_SF_LOOP;
                chan->statusFlags = status;
            }

            goto attack;
        }

        envelopeVolume = chan->envelopeVolume;

        if (status & SOUND_CHANNEL_SF_IEC)
        {
            /* Pseudo echo: a fixed-length tail at a fixed volume. */
            if (--chan->pseudoEchoLength == 0)
            {
                chan->statusFlags = 0;
                continue;
            }
        }
        else if (status & SOUND_CHANNEL_SF_STOP)
        {
            envelopeVolume = (envelopeVolume * chan->release) >> 8;

            if (envelopeVolume <= chan->pseudoEchoVolume)
                goto pseudoEcho;
        }
        else
        {
            u32 phase = status & SOUND_CHANNEL_SF_ENV;

            if (phase == SOUND_CHANNEL_SF_ENV_DECAY)
            {
                envelopeVolume = (envelopeVolume * chan->decay) >> 8;

                if (envelopeVolume <= chan->sustain)
                {
                    envelopeVolume = chan->sustain;

                    if (envelopeVolume == 0)
                        goto pseudoEcho;

                    chan->statusFlags = --status;   /* decay -> sustain */
                }
            }
            else if (phase == SOUND_CHANNEL_SF_ENV_ATTACK)
            {
            attack:
                envelopeVolume += chan->attack;

                if (envelopeVolume >= 0xFF)
                {
                    envelopeVolume = 0xFF;
                    chan->statusFlags = --status;   /* attack -> decay */
                }
            }
            /* sustain and release hold the volume they have */
        }

        goto setVolume;

    pseudoEcho:
        envelopeVolume = chan->pseudoEchoVolume;

        if (envelopeVolume == 0)
        {
            chan->statusFlags = 0;
            continue;
        }

        chan->statusFlags = status | SOUND_CHANNEL_SF_IEC;

    setVolume:
        chan->envelopeVolume = (u8)envelopeVolume;

        /* The master volume folds in here, one step above the per-channel
         * panning, so the mixer only ever reads the two finished values. */
        u32 scaled = (envelopeVolume * (masterVolume + 1)) >> 4;
        chan->envelopeVolumeRight = (u8)((chan->rightVolume * scaled) >> 8);
        chan->envelopeVolumeLeft  = (u8)((chan->leftVolume  * scaled) >> 8);
    }

    soundInfo->ident = ID_NUMBER;
}
