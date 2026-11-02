/* setBfree - DSP tonewheel organ
 *
 * Copyright (C) 2003-2004 Fredrik Kilander <fk@dsv.su.se>
 * Copyright (C) 2008-2018 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2012 Will Panther <pantherb@setbfree.org>
 * Copyright (C) 2026 Jacob Schooley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* One complete organ (tone generator, scanner, preamp, reverb, Leslie) without
 * a front end, the way b_synth/lv2.c sets one up. Creating and destroying
 * allocate; everything else is real-time safe. */

#ifndef SBF_ENGINE_H
#define SBF_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBF_FRAGMENT 128 /* BUFFER_SIZE_SAMPLES */

typedef struct sbf_engine sbf_engine;

/* config: setBfree configuration lines ("key=value\n"), applied before the
 * engine initialises, or NULL. Numbers are read in the C locale whatever the
 * process's locale. Returns NULL when out of memory. */
sbf_engine* sbf_create (double rate, const char* config);
void        sbf_destroy (sbf_engine* e);

/* A MIDI message as setBfree's front ends pass it (notes, CCs, program changes). */
void sbf_midi (sbf_engine* e, const uint8_t* msg, size_t size);

/* A control function by its index (sbf_control_id), value 0..127. */
int  sbf_control_id (const char* name);
void sbf_control (sbf_engine* e, int id, uint8_t value);

/* Keyboard splits on the upper manual's channel: keys below pedalSplit play the pedals,
 * keys from there below lowerSplit the lower manual; 0 turns a split off. */
void sbf_keyboard_split (sbf_engine* e, int pedalSplit, int lowerSplit);

/* setBfree's built-in program (pgm/default.pgm) on MIDI program pc, 0..127. Fields the
 * program does not set are -1. Returns 0 for unused programs, programs that set no upper
 * drawbars (modifiers such as "Percussion ON") and programs with random drawbars. */
typedef struct sbf_program {
	char   name[32];
	int    upper[9], lower[9], pedal[9]; /* drawbar settings 0..8 */
	int    vibrato;                     /* 0..5: V1 C1 V2 C2 V3 C3 */
	int    vibratoUpper, vibratoLower, percussion, percussionSoft, percussionFast, percussionSecond, overdrive;
	int    leslie;                      /* 0 stop, 1 slow, 2 fast */
	double reverb;                      /* 0..1 */
	int    pedalSplit, lowerSplit, transpose, pedalTranspose, lowerTranspose, upperTranspose;
} sbf_program;
int sbf_program_get (int pc, sbf_program* out);

/* Releases every key. */
void sbf_panic (sbf_engine* e);

/* Renders the next SBF_FRAGMENT frames of stereo output. */
void sbf_render (sbf_engine* e, float* left, float* right);

/* Keys currently down in the tone generator. */
int sbf_keys_down (const sbf_engine* e);

#ifdef __cplusplus
}
#endif
#endif
