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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#include "cfgParser.h"
#include "global_inst.h"
#include "main.h"
#include "midi.h"
#include "program.h"
#include "state.h"
#include "vibrato.h"
#include "whirl.h"

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

/* Transpose slots of Programme.transpose (private to src/program.c). */
enum { PGM_TRANSP = 0, PGM_CHA_UM = 4, PGM_CHA_LM = 5, PGM_CHA_PD = 6 };

int
sbf_program_get (int pc, sbf_program* out)
{
	struct b_programme* progs = allocProgs ();
	int                 p     = pc + (progs ? progs->MIDIControllerPgmOffset : 0);
	if (!progs || pc < 0 || p <= 0 || p >= MAXPROGS) {
		freeProgs (progs);
		return 0;
	}
	const Programme*   g = &progs->programmes[p];
	const unsigned int f = g->flags[0];
	if (!(f & FL_INUSE) || !(f & FL_DRAWBR) || (f & FL_DRWRND)) {
		freeProgs (progs);
		return 0;
	}
	memset (out, -1, sizeof (*out));
	out->reverb = -1;
	snprintf (out->name, sizeof (out->name), "%s", g->name);
	for (int i = 0; i < 9; ++i) {
		out->upper[i] = (int)g->drawbars[i];
		if (f & FL_LOWDRW)
			out->lower[i] = (int)g->lowerDrawbars[i];
		if (f & FL_PDLDRW)
			out->pedal[i] = (int)g->pedalDrawbars[i];
	}
	/* as installProgram () in src/program.c applies them */
	if (f & FL_SCANNR)
		out->vibrato = ((g->scanner & 0xf) << 1) - ((g->scanner & CHO_) ? 1 : 2);
	if (f & FL_VCRUPR)
		out->vibratoUpper = (g->scanner & 0x200) ? 1 : 0;
	if (f & FL_VCRLWR)
		out->vibratoLower = (g->scanner & 0x100) ? 1 : 0;
	if (f & FL_PRCENA)
		out->percussion = g->percussionEnabled ? 1 : 0;
	if (f & FL_PRCVOL)
		out->percussionSoft = g->percussionVolume ? 1 : 0;
	if (f & FL_PRCSPD)
		out->percussionFast = g->percussionSpeed ? 1 : 0;
	if (f & FL_PRCHRM)
		out->percussionSecond = g->percussionHarmonic ? 1 : 0;
	if (f & FL_OVRSEL)
		out->overdrive = g->overdriveSelect ? 1 : 0;
	if (f & FL_ROTSPS) /* rotary.speed-preset: 0 slow, 64 stop, 127 fast */
		out->leslie = g->rotarySpeedSelect == 0 ? 1 : g->rotarySpeedSelect == 1 ? 0 : 2;
	if (f & FL_RVBMIX)
		out->reverb = g->reverbMix;
	if (f & FL_KSPLTP)
		out->pedalSplit = g->keyboardSplitPedals;
	if (f & FL_KSPLTL)
		out->lowerSplit = g->keyboardSplitLower;
	if (f & FL_TRANSP)
		out->transpose = g->transpose[PGM_TRANSP];
	if (f & FL_TRA_PD)
		out->pedalTranspose = g->transpose[PGM_CHA_PD];
	if (f & FL_TRA_LM)
		out->lowerTranspose = g->transpose[PGM_CHA_LM];
	if (f & FL_TRA_UM)
		out->upperTranspose = g->transpose[PGM_CHA_UM];
	freeProgs (progs);
	return 1;
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

int
sbf_release_frames (const sbf_engine* e)
{
#ifdef SETBFREE_STOCK
	return SBF_FRAGMENT;
#else
	return e->inst.synth->envSpan;
#endif
}

#ifndef SETBFREE_STOCK /* the rotor needs whirlRamp, which upstream does not have */
struct sbf_rotor {
	void*           state;
	void*           midicfg; /* the control functions whirl.c registers */
	struct b_whirl* whirl;
	double          rate;
	uint32_t        phase; /* samples into the current fragment */
	float           drumL[SBF_FRAGMENT], drumR[SBF_FRAGMENT];
};

void
sbf_rotor_destroy (sbf_rotor* r)
{
	if (!r)
		return;
	if (r->whirl)
		freeWhirl (r->whirl);
	if (r->midicfg)
		freeMidiCfg (r->midicfg);
	if (r->state)
		freeRunningConfig (r->state);
	free (r);
}

sbf_rotor*
sbf_rotor_create (double rate, const char* config)
{
	sbf_rotor* r = (sbf_rotor*)calloc (1, sizeof (sbf_rotor));
	if (!r)
		return NULL;
	r->rate  = rate;
	r->state = allocRunningConfig ();
	r->whirl = allocWhirl ();
	if (r->state)
		r->midicfg = allocMidiCfg (r->state);
	if (!r->state || !r->whirl || !r->midicfg) {
		sbf_rotor_destroy (r);
		return NULL;
	}
	initControllerTable (r->midicfg);
	midiPrimeControllerMapping (r->midicfg);

	locale_t c   = newlocale (LC_NUMERIC_MASK, "C", (locale_t)0);
	locale_t old = c ? uselocale (c) : (locale_t)0;
	const char* line = config;
	while (line && *line) {
		const char* end = strchr (line, '\n');
		size_t      n   = end ? (size_t)(end - line) : strlen (line);
		char        kv[1024];
		char*       value;
		if (n < sizeof (kv)) {
			memcpy (kv, line, n);
			kv[n] = 0;
			if ((value = strchr (kv, '='))) {
				ConfigContext cfg = { "", 0, kv, value + 1 };
				*value            = 0;
				whirlConfig (r->whirl, &cfg);
			}
		}
		line = end ? end + 1 : NULL;
	}
	initWhirl (r->whirl, r->midicfg, rate);
	r->whirl->rampByCaller = 1;
	if (c) {
		uselocale (old);
		freelocale (c);
	}
	return r;
}

void
sbf_rotor_speed (sbf_rotor* r, int speed)
{
	setRevSelect (r->whirl, speed == 0 ? WHIRL_STOP : speed == 2 ? WHIRL_FAST : WHIRL_SLOW);
}

void
sbf_rotor_follow (sbf_rotor* r, const sbf_rotor* from)
{
	r->whirl->hornAngleGRD = from->whirl->hornAngleGRD;
	r->whirl->drumAngleGRD = from->whirl->drumAngleGRD;
	r->whirl->hornIncr     = from->whirl->hornIncr;
	r->whirl->drumIncr     = from->whirl->drumIncr;
	r->phase               = from->phase;
	setRevSelect (r->whirl, r->whirl->revSelect); /* accelerate or brake from there */
}

void
sbf_rotor_clear (sbf_rotor* r)
{
	struct b_whirl* w = r->whirl;
	memset (w->HLbuf, 0, sizeof (w->HLbuf));
	memset (w->HRbuf, 0, sizeof (w->HRbuf));
	memset (w->DLbuf, 0, sizeof (w->DLbuf));
	memset (w->DRbuf, 0, sizeof (w->DRbuf));
	memset (w->adx0, 0, sizeof (w->adx0));
	memset (w->adx1, 0, sizeof (w->adx1));
	memset (w->adx2, 0, sizeof (w->adx2));
	memset (w->z, 0, sizeof (w->z));
	/* the filters' two state words after their coefficients (whirl.h, filterCoeff) */
	w->drfL[z0] = w->drfL[z1] = w->drfR[z0] = w->drfR[z1] = 0;
	w->hafw[z0] = w->hafw[z1] = w->hbfw[z0] = w->hbfw[z1] = 0;
}

/* The speeds move once per fragment, as in the organ, whatever lengths the caller renders. */
void
sbf_rotor_process (sbf_rotor* r, const float* in, float* left, float* right, uint32_t n)
{
	while (n) {
		const uint32_t k = n < SBF_FRAGMENT - r->phase ? n : SBF_FRAGMENT - r->phase;
		if (r->phase == 0)
			whirlRamp (r->whirl, SBF_FRAGMENT);
		whirlProc3 (r->whirl, in, left, right, r->drumL, r->drumR, k);
		in += k;
		left += k;
		right += k;
		n -= k;
		r->phase = (r->phase + k) % SBF_FRAGMENT;
	}
}

double
sbf_rotor_horn_rpm (const sbf_rotor* r)
{
	return r->whirl->hornIncr * r->rate * 60.0;
}
#endif
