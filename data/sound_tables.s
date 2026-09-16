@ Sound data that has to live in RDRAM.
@
@ The engine reads all of this a byte or a halfword at a time -- gSongTable's
@ music-player index, a key split table's instrument number, the 4-bit samples
@ of a programmable wave -- and cartridge ROM cannot serve sub-word reads
@ correctly. It is small enough to fit, unlike the samples and voicegroups in
@ data/sound_data.s.

	.section .rodata

	.include "asm/macros/m4a.inc"
	.include "asm/macros/music_voice.inc"

	.include "sound/programmable_wave_data.inc"
	.include "sound/music_player_table.inc"
	.include "sound/song_table.inc"

	.align 2
