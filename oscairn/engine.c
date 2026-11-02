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

#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#include <xlocale.h>
#endif

#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#include "cfgParser.h"
#include "global_inst.h"
#include "main.h"
#include "midi.h"
#include "program.h"
#include "state.h"

/* cfgParser.c asks the front end for its own settings; there are none. */
int
mainConfig (ConfigContext* cfg)
{
	return 0;
}

const ConfigDoc*
mainDoc ()
{
	return NULL;
}

#ifdef SETBFREE_STOCK
/* Upstream's tone generator reads the rate from this global (the stock-vs-fork gate). */
double SampleRateD = 48000.0;
#endif

struct sbf_engine {
	struct b_instance inst;
	float             a[SBF_FRAGMENT], b[SBF_FRAGMENT], c[SBF_FRAGMENT];
	float             drumL[SBF_FRAGMENT], drumR[SBF_FRAGMENT];
};

static void
freeInstance (struct b_instance* inst)
{
	if (inst->reverb)
		freeReverb (inst->reverb);
	if (inst->whirl)
		freeWhirl (inst->whirl);
	if (inst->synth)
		freeToneGenerator (inst->synth);
	if (inst->midicfg)
		freeMidiCfg (inst->midicfg);
	if (inst->preamp)
		freePreamp (inst->preamp);
	if (inst->progs)
		freeProgs (inst->progs);
	if (inst->state)
		freeRunningConfig (inst->state);
}

/* Each "key=value" line of config, as b_synth/lv2.c restores its state's C lines. */
static void
applyConfig (struct b_instance* inst, const char* config)
{
	const char* line = config;
	while (line && *line) {
		const char* end = strchr (line, '\n');
		size_t      n   = end ? (size_t)(end - line) : strlen (line);
		char        kv[1024];
		if (n < sizeof (kv)) {
			char* value;
			memcpy (kv, line, n);
			kv[n] = 0;
			if ((value = strchr (kv, '='))) {
				*value++ = 0;
				evaluateConfigKeyValue (inst, kv, value);
			}
		}
		line = end ? end + 1 : NULL;
	}
}

sbf_engine*
sbf_create (double rate, const char* config)
{
	sbf_engine* e = (sbf_engine*)calloc (1, sizeof (sbf_engine));
	if (!e)
		return NULL;
	struct b_instance* inst = &e->inst;

	/* allocSynth () in b_synth/lv2.c */
	inst->state  = allocRunningConfig ();
	inst->progs  = allocProgs ();
	inst->reverb = allocReverb ();
	inst->whirl  = allocWhirl ();
	if (inst->state)
		inst->midicfg = allocMidiCfg (inst->state);
#ifdef SETBFREE_STOCK
	SampleRateD  = rate;
	inst->synth  = allocTonegen ();
#else
	inst->synth  = allocTonegen (rate);
#endif
	inst->preamp = allocPreamp ();
	if (!inst->state || !inst->progs || !inst->reverb || !inst->whirl || !inst->midicfg || !inst->synth || !inst->preamp) {
		freeInstance (inst);
		free (e);
		return NULL;
	}
	initControllerTable (inst->midicfg);
	midiPrimeControllerMapping (inst->midicfg);

	/* The configuration's numbers are read with strtod and sscanf: the C locale
	 * for this thread only (setlocale would change the whole process). */
	locale_t c   = newlocale (LC_NUMERIC_MASK, "C", (locale_t)0);
	locale_t old = c ? uselocale (c) : (locale_t)0;

	applyConfig (inst, config);

	/* initSynth () in b_synth/lv2.c */
	unsigned int defaultPreset[9] = { 8, 8, 6, 0, 0, 0, 0, 0, 0 };
	initToneGenerator (inst->synth, inst->midicfg);
	initVibrato (inst->synth, inst->midicfg);
	initPreamp (inst->preamp, inst->midicfg);
	initReverb (inst->reverb, inst->midicfg, rate);
	initWhirl (inst->whirl, inst->midicfg, rate);
	initRunningConfig (inst->state, inst->midicfg);
	initMidiTables (inst->midicfg);
	setDrawBars (inst, 0, defaultPreset);
	/* tonegen.c starts its output gain at 1.0 and glides to the swell pedal's (0.07) with
	 * a ~25 Hz low-pass: the first notes after initialisation came out up to 14 times too loud. */
	inst->synth->currentGain = inst->synth->swellPedalGain;

	if (c) {
		uselocale (old);
		freelocale (c);
	}
	return e;
}

void
sbf_destroy (sbf_engine* e)
{
	if (!e)
		return;
	freeInstance (&e->inst);
	free (e);
}

void
sbf_midi (sbf_engine* e, const uint8_t* msg, size_t size)
{
	parse_raw_midi_data (&e->inst, (uint8_t*)msg, size);
}

int
sbf_control_id (const char* name)
{
	return getCCFunctionId (name);
}

void
sbf_control (sbf_engine* e, int id, uint8_t value)
{
	if (id >= 0 && id < getCCFunctionCount () - 1)
		callMIDIControlFunction (e->inst.midicfg, getCCFunctionName (id), value);
}

void
sbf_keyboard_split (sbf_engine* e, int pedalSplit, int lowerSplit)
{
	setKeyboardSplitMulti (e->inst.midicfg, 3, pedalSplit, lowerSplit, 0, 0, 0);
}

void
sbf_panic (sbf_engine* e)
{
	midi_panic (&e->inst);
}

void
sbf_render (sbf_engine* e, float* left, float* right)
{
	/* synthSound () in b_synth/lv2.c */
	oscGenerateFragment (e->inst.synth, e->a, SBF_FRAGMENT);
	preamp (e->inst.preamp, e->a, e->b, SBF_FRAGMENT);
	reverb (e->inst.reverb, e->b, e->c, SBF_FRAGMENT);
	whirlProc3 (e->inst.whirl, e->c, left, right, e->drumL, e->drumR, SBF_FRAGMENT);
}

int
sbf_keys_down (const sbf_engine* e)
{
	int n = 0;
	for (int k = 0; k < MAX_KEYS; ++k)
		n += e->inst.synth->activeKeys[k] != 0;
	return n;
}
