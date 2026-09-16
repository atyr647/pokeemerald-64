@ Sound data that stays in cartridge ROM.
@
@ tools/n64_rodata_split.py keeps this whole object out of RDRAM, because the
@ samples alone are 2.2 MB and the voicegroups another 244 KB -- together more
@ than the RDRAM window for read-only data can hold. Everything here is
@ therefore read through word-sized loads: see N64_ReadRomByte in
@ include/n64/defines.h, and its callers in src/n64/audio.c (sample fetch and
@ DPCM decode) and src/n64/m4a_core.c (voice lookup).
@
@ The key split tables are here too, not because of their size but because the
@ macro that builds them assigns absolute symbols that are not global, so they
@ have to be in the same object as the voicegroups that name them.
@
@ The smaller tables that are read field by field live in data/sound_tables.s
@ instead, which does land in RDRAM.

	.section .rodata

	.include "asm/macros/m4a.inc"
	.include "asm/macros/music_voice.inc"

	.include "sound/keysplit_tables.inc"
	.include "sound/voice_groups.inc"
	.include "sound/direct_sound_data.inc"

	.align 2
