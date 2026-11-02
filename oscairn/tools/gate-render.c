/* setBfree - DSP tonewheel organ
 *
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

/* The stock-vs-fork gate's performance: the same MIDI and control changes at the
 * same fragments, rendered by whichever engine sources this was built against,
 * written as raw interleaved float32.
 *   setbfree-gate-render RATE OUT.raw [config|noconfig] [MORE-CONFIG]
 * MORE-CONFIG: further configuration lines, for settings only the fork has.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

static void
midi3 (sbf_engine* e, int a, int b, int c)
{
	uint8_t m[3] = { (uint8_t)a, (uint8_t)b, (uint8_t)c };
	sbf_midi (e, m, 3);
}

static void
control (sbf_engine* e, const char* name, int value)
{
	int id = sbf_control_id (name);
	if (id < 0) {
		fprintf (stderr, "no control %s\n", name);
		exit (1);
	}
	sbf_control (e, id, (uint8_t)value);
}

/* What happens at the start of fragment f. */
static void
perform (sbf_engine* e, int f, int fragmentsPerSecond)
{
	const int s = fragmentsPerSecond;
	if (f == 0) {
		control (e, "percussion.enable", 127);
		control (e, "percussion.harmonic", 127);
		control (e, "vibrato.knob", 127);
		control (e, "vibrato.upper", 127);
		control (e, "reverb.mix", 40);
		midi3 (e, 0x90, 60, 100); /* upper chord */
		midi3 (e, 0x90, 64, 100);
		midi3 (e, 0x90, 67, 100);
		midi3 (e, 0x91, 48, 100); /* lower */
		midi3 (e, 0x92, 36, 100); /* pedal */
	}
	if (f == s / 2) {
		control (e, "upper.drawbar4", 0); /* MIDI 0 is drawbar 8 */
		control (e, "lower.drawbar8", 0);
		control (e, "pedal.drawbar16", 20);
	}
	if (f == s) {
		control (e, "rotary.speed-preset", 127); /* fast */
		control (e, "overdrive.enable", 127);
		control (e, "overdrive.character", 90);
		control (e, "swellpedal1", 100);
	}
	if (f == 2 * s) {
		midi3 (e, 0x80, 64, 0);
		midi3 (e, 0x90, 65, 90);
		midi3 (e, 0xB0, 1, 127);
	}
	if (f == 3 * s) {
		control (e, "rotary.speed-preset", 0); /* slow */
		uint8_t pc[2] = { 0xC0, 5 };
		sbf_midi (e, pc, 2);
		midi3 (e, 0x90, 72, 127);
	}
	if (f == 4 * s) {
		midi3 (e, 0x80, 60, 0);
		midi3 (e, 0x80, 65, 0);
		midi3 (e, 0x80, 67, 0);
		midi3 (e, 0x80, 72, 0);
		midi3 (e, 0x81, 48, 0);
		midi3 (e, 0x82, 36, 0);
	}
}

int
main (int argc, char** argv)
{
	if (argc < 3) {
		fprintf (stderr, "usage: %s RATE OUT.raw [config|noconfig] [MORE-CONFIG]\n", argv[0]);
		return 2;
	}
	const double rate   = atof (argv[1]);
	const char*  config = (argc > 3 && !strcmp (argv[3], "config"))
	                         ? "osc.tuning=442\nosc.compartment-crosstalk=0.02\nwhirl.horn.fastrpm=400\nwhirl.drum.slowrpm=40\nosc.perc.fast=0.8\nscanner.hz=7.0\n"
	                         : "";
	char text[1024];
	snprintf (text, sizeof (text), "%s%s", config, argc > 4 ? argv[4] : "");
	sbf_engine* e = sbf_create (rate, *text ? text : NULL);
	if (!e) {
		fprintf (stderr, "engine failed\n");
		return 1;
	}
	FILE* out = fopen (argv[2], "wb");
	if (!out) {
		perror (argv[2]);
		return 1;
	}
	const int fps = (int)(rate / SBF_FRAGMENT);
	float     l[SBF_FRAGMENT], r[SBF_FRAGMENT], lr[2 * SBF_FRAGMENT];
	for (int f = 0; f < 6 * fps; ++f) {
		perform (e, f, fps);
		sbf_render (e, l, r);
		for (int i = 0; i < SBF_FRAGMENT; ++i) {
			lr[2 * i]     = l[i];
			lr[2 * i + 1] = r[i];
		}
		fwrite (lr, sizeof (float), 2 * SBF_FRAGMENT, out);
	}
	fclose (out);
	sbf_destroy (e);
	return 0;
}
