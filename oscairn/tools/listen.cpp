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

// Renders the listening examples (doc/oscairn-plan.md, section 5) as 32-bit float WAVs:
//   organ-listen OUTDIR
// Each example is the plugin with some parameters set (or a preset loaded) playing a phrase.

#include "harness.h"

#include "engine.h"
#include "leslie.h"

#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace organtest;

namespace
{
    clap_id param(Instance& o, const char* name)
    {
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        for(uint32_t i = 0, n = params->count(o.p); i < n; ++i)
        {
            clap_param_info_t info;
            params->get_info(o.p, i, &info);
            if(!std::strcmp(info.name, name)) return info.id;
        }
        std::fprintf(stderr, "no parameter %s\n", name);
        std::exit(2);
    }

    void writeWav(const std::string& path, const Output& out, double rate)
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if(!f) { std::perror(path.c_str()); std::exit(1); }
        const uint32_t frames = uint32_t(out.left.size()), bytes = frames * 8;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(3); u16(2); u32(uint32_t(rate)); u32(uint32_t(rate) * 8); u16(8); u16(32);
        std::fwrite("data", 1, 4, f); u32(bytes);
        for(uint32_t i = 0; i < frames; ++i) { std::fwrite(&out.left[i], 4, 1, f); std::fwrite(&out.right[i], 4, 1, f); }
        std::fclose(f);
    }

    using Setup = std::vector<std::pair<const char*, double>>;
    using Play = std::function<void(Instance&, double)>; // schedules notes; returns nothing

    // Notes on channel 1 (upper manual): (start s, length s, key).
    void notes(Instance& o, double rate, const std::vector<std::tuple<double, double, int>>& list)
    {
        int id = 1;
        for(const auto& [start, length, key] : list)
        {
            o.at(uint32_t(start * rate)).note(CLAP_EVENT_NOTE_ON, 0, int16_t(key), id);
            o.at(uint32_t((start + length) * rate)).note(CLAP_EVENT_NOTE_OFF, 0, int16_t(key), id);
            ++id;
        }
    }
    // A held chord, then a short melody over it.
    void phrase(Instance& o, double rate)
    {
        notes(o, rate, {{0, 3.5, 48}, {0, 3.5, 55}, {0, 3.5, 58}, {0, 3.5, 64},
                        {0.5, 0.25, 72}, {0.75, 0.25, 74}, {1.0, 0.5, 75}, {1.5, 0.25, 74}, {1.75, 0.25, 72}, {2.0, 1.0, 70}});
    }
    // Detached chords, for key click.
    void staccato(Instance& o, double rate)
    {
        for(int i = 0; i < 6; ++i) notes(o, rate, {{i * 0.5, 0.15, 60}, {i * 0.5, 0.15, 64}, {i * 0.5, 0.15, 67}});
    }
    // Slow chords, for leakage: a quiet passage where crosstalk is audible.
    void slowChords(Instance& o, double rate)
    {
        notes(o, rate, {{0, 2, 53}, {0, 2, 57}, {0, 2, 60}, {2, 2, 52}, {2, 2, 55}, {2, 2, 60}});
    }
    // Percussion: legato (no retrigger) then detached (each note strikes).
    void percussion(Instance& o, double rate)
    {
        notes(o, rate, {{0, 0.55, 60}, {0.5, 0.55, 64}, {1.0, 0.55, 67}, {1.5, 0.6, 72}, {2.5, 0.3, 60}, {3.0, 0.3, 64}, {3.5, 0.3, 67}, {4.0, 0.6, 72}});
    }
    // A chord with the Leslie going slow → fast → slow.
    void leslieRamp(Instance& o, double rate)
    {
        notes(o, rate, {{0, 9, 60}, {0, 9, 64}, {0, 9, 67}, {0, 9, 72}});
        o.at(uint32_t(2 * rate)).param(param(o, "Leslie speed"), 2);
        o.at(uint32_t(5.5 * rate)).param(param(o, "Leslie speed"), 1);
    }
    // A full chord with the swell (expression) pedal opening, overdrive on.
    void swell(Instance& o, double rate)
    {
        notes(o, rate, {{0, 5, 48}, {0, 5, 55}, {0, 5, 60}, {0, 5, 63}, {0, 5, 67}});
        for(int i = 0; i <= 40; ++i) o.at(uint32_t((0.5 + i * 0.1) * rate)).midi(0xB0, 11, uint8_t(40 + i * 87 / 40));
    }

    // The engine alone (no plugin), for settings the plugin does not offer: the key click and
    // release in one 128-sample fragment at every rate (osc.envelope.rate=0), as upstream.
    Output engineStaccato(double rate, const char* config)
    {
        sbf_engine* e = sbf_create(rate, config);
        sbf_control(e, sbf_control_id("rotary.speed-preset"), 64); // Leslie stopped: the clicks unmasked
        Output out;
        float l[SBF_FRAGMENT], r[SBF_FRAGMENT];
        const uint32_t fragments = uint32_t(3.5 * rate) / SBF_FRAGMENT;
        for(uint32_t f = 0; f < fragments; ++f)
        {
            const double t = double(f) * SBF_FRAGMENT / rate;
            const double at = std::fmod(t, 0.5);
            if(at < SBF_FRAGMENT / rate || (at >= 0.15 && at < 0.15 + SBF_FRAGMENT / rate))
                for(uint8_t k : {60, 64, 67}) { const uint8_t m[3] = {uint8_t(at < 0.15 ? 0x90 : 0x80), k, 127}; sbf_midi(e, m, 3); }
            sbf_render(e, l, r);
            out.left.insert(out.left.end(), l, l + SBF_FRAGMENT);
            out.right.insert(out.right.end(), r, r + SBF_FRAGMENT);
        }
        sbf_destroy(e);
        return out;
    }

    struct Example
    {
        std::string file;
        Setup setup;
        Play play;
        double seconds;
        double rate = 48000;
        std::string preset; // a program's MIDI number
    };
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR\n", argv[0]); return 2; }
    const std::string dir = argv[1];
    const Setup full = {{"Upper 16'", 8}, {"Upper 5 1/3'", 8}, {"Upper 8'", 8}, {"Upper 4'", 8}, {"Upper 2 2/3'", 8}, {"Upper 2'", 8}, {"Upper 1 3/5'", 8}, {"Upper 1 1/3'", 8}, {"Upper 1'", 8}};
    auto with = [](Setup a, const Setup& b) { a.insert(a.end(), b.begin(), b.end()); return a; };
    std::vector<Example> examples = {
        {"01-default.wav", {}, phrase, 5},
        {"02-click-off.wav", {{"Key click", 0}, {"Release click", 0}}, staccato, 3.5},
        {"02-click-default.wav", {}, staccato, 3.5},
        {"02-click-high.wav", {{"Key click", 1}, {"Release click", 0.75}}, staccato, 3.5},
        {"03-leakage-off.wav", {{"Compartment crosstalk", 0}, {"Terminal strip crosstalk", 0}, {"Wiring crosstalk", 0}, {"Leslie speed", 0}}, slowChords, 4.5},
        {"03-leakage-default.wav", {{"Leslie speed", 0}}, slowChords, 4.5},
        {"03-leakage-high.wav", {{"Compartment crosstalk", 0.05}, {"Transformer crosstalk", 0.02}, {"Terminal strip crosstalk", 0.05}, {"Wiring crosstalk", 0.05}, {"Leslie speed", 0}}, slowChords, 4.5},
        {"04-percussion-third-fast-soft.wav", {{"Upper 16'", 8}, {"Upper 5 1/3'", 8}, {"Upper 8'", 8}, {"Percussion", 1}, {"Percussion decay", 1}, {"Percussion volume", 1}}, percussion, 5},
        {"04-percussion-second-slow-normal.wav", {{"Upper 16'", 8}, {"Upper 5 1/3'", 8}, {"Upper 8'", 8}, {"Percussion", 1}, {"Percussion harmonic", 1}}, percussion, 5.5},
        {"05-vibrato-c3.wav", {{"Vibrato upper", 1}, {"Vibrato", 5}, {"Leslie speed", 0}}, phrase, 4},
        {"05-vibrato-v3.wav", {{"Vibrato upper", 1}, {"Vibrato", 4}, {"Leslie speed", 0}}, phrase, 4},
        {"05-vibrato-c1.wav", {{"Vibrato upper", 1}, {"Vibrato", 1}, {"Leslie speed", 0}}, phrase, 4},
        {"06-overdrive-swell.wav", with(full, {{"Overdrive", 1}, {"Overdrive drive", 0.5}, {"Overdrive character", 0.6}}), swell, 6},
        {"07-leslie-ramp.wav", {{"Upper 4'", 8}}, leslieRamp, 10},
        {"07-leslie-ramp-slow-motors.wav", {{"Upper 4'", 8}, {"Horn acceleration", 0.6}, {"Horn deceleration", 1.2}, {"Drum acceleration", 6}, {"Drum deceleration", 3}}, leslieRamp, 10},
        {"07-leslie-off.wav", {{"Upper 4'", 8}, {"Leslie", 0}}, leslieRamp, 10},
        {"08-wheels-sine.wav", {}, phrase, 4},
        {"08-wheels-triangle.wav", {{"Wheel shape", 1}}, phrase, 4},
        {"08-wheels-bassy.wav", {{"Wheel EQ low level", 0.9}, {"Wheel EQ low slope", 1}, {"Wheel EQ high level", 0.3}}, phrase, 4},
        {"08-wheels-trebly.wav", {{"Wheel EQ low level", 0.2}, {"Wheel EQ high level", 1.5}}, phrase, 4},
        {"08-wheels-scoop.wav", {{"Wheel EQ low slope", -2}, {"Wheel EQ high slope", 2}}, phrase, 4},
        {"10-rate-44100.wav", {{"Percussion", 1}}, staccato, 3.5, 44100},
        {"10-rate-48000.wav", {{"Percussion", 1}}, staccato, 3.5, 48000},
        {"10-rate-88200.wav", {{"Percussion", 1}}, staccato, 3.5, 88200},
        {"10-rate-96000.wav", {{"Percussion", 1}}, staccato, 3.5, 96000},
        {"10-rate-192000.wav", {{"Percussion", 1}}, staccato, 3.5, 192000},
    };
    // A few of setBfree's programs, by MIDI program number (see the README for names).
    for(int pc : {0, 4, 6, 7, 11, 14, 15, 16, 18})
        examples.push_back({"09-preset-" + std::to_string(pc) + ".wav", {}, phrase, 5, 48000, std::to_string(pc)});

    for(auto& e : examples)
    {
        Instance o(e.rate);
        if(!e.preset.empty())
        {
            auto* load = o.ext<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD);
            if(!load->from_location(o.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, e.preset.c_str())) { std::fprintf(stderr, "no preset %s\n", e.preset.c_str()); return 1; }
        }
        for(const auto& [name, value] : e.setup) o.at(0).param(param(o, name), value);
        o.render(512, 512);
        o.service(); // configuration parameters build their engine
        o.render(uint32_t(e.rate / 2), 512); // the crossfade and the Leslie settle
        e.play(o, e.rate);
        const auto out = o.render(uint32_t(e.seconds * e.rate), 512);
        writeWav(dir + "/" + e.file, out, e.rate);
        double peak = 0;
        for(float v : out.left) peak = std::max(peak, double(std::fabs(v)));
        std::printf("%s: peak %.3f\n", e.file.c_str(), peak);
    }

    // Key click at each rate, high click so it is easy to hear: as it is now (2.9 ms at every
    // rate) and as upstream's (one fragment), from the same engine.
    for(double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        for(bool fragment : {false, true})
        {
            const std::string name = "11-click-" + std::to_string(int(rate)) + (fragment ? "-one-fragment.wav" : ".wav");
            const std::string config = std::string("osc.attack.click.level=1\nosc.release.click.level=0.75\n") + (fragment ? "osc.envelope.rate=0\n" : "");
            writeWav(dir + "/" + name, engineStaccato(rate, config.c_str()), rate);
            std::printf("%s\n", name.c_str());
        }

    // setBfree Leslie, the effect: the organ dry (its own Leslie off), then through the effect,
    // the mod wheel going fast at 2 s and back at 5.5 s; a sawtooth chord; at 192 kHz.
    for(double rate : {48000.0, 192000.0})
    {
        Instance organ(rate);
        organ.at(0).param(param(organ, "Leslie"), 0).param(param(organ, "Upper 4'"), 8);
        organ.render(512, 512);
        organ.service();
        organ.render(uint32_t(rate / 2), 512);
        notes(organ, rate, {{0, 9, 60}, {0, 9, 64}, {0, 9, 67}, {0, 9, 72}});
        const auto dry = organ.render(uint32_t(10 * rate), 512);
        const std::string suffix = rate == 48000 ? "" : "-" + std::to_string(int(rate));
        if(rate == 48000) writeWav(dir + "/12-leslie-effect-dry-organ.wav", dry, rate);
        struct Source { std::string file; std::function<void(uint64_t, float&, float&)> input; };
        std::vector<Source> sources = {
            {"12-leslie-effect-organ" + suffix + ".wav", [&dry](uint64_t i, float& l, float& r) { l = i < dry.left.size() ? dry.left[i] : 0.f; r = i < dry.right.size() ? dry.right[i] : 0.f; }},
        };
        if(rate == 48000)
            sources.push_back({"12-leslie-effect-saw.wav", [rate](uint64_t i, float& l, float& r) {
                const double t = double(i) / rate;
                double x = 0;
                for(double hz : {130.81, 155.56, 196.0, 233.08}) x += 2 * std::fmod(t * hz, 1.0) - 1; // C minor 7, sawtooth
                l = r = t < 9 ? float(0.08 * x) : 0.f;
            }});
        for(auto& src : sources)
        {
            Instance fx(rate, true, leslie::create);
            fx.input = src.input;
            fx.at(uint32_t(2 * rate)).midi(0xB0, 1, 127).at(uint32_t(5.5 * rate)).midi(0xB0, 1, 0);
            writeWav(dir + "/" + src.file, fx.render(uint32_t(10 * rate), 512), rate);
            std::printf("%s\n", src.file.c_str());
        }
    }
    return 0;
}
