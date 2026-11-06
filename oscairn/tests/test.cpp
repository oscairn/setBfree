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

// Tests of the setBfree plugin through its CLAP interface with a minimal host
// (doc/oscairn-plan.md, section 4). Run with a test name, or "all"; leslie_test.cpp has
// the Leslie effect's.

#include "harness.h"

#include "engine.h"

#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <time.h>
#include <unistd.h>

using namespace organtest;

namespace
{
    uint32_t param(const char* name)
    {
        auto* p = organ::create(&host.host);
        p->init(p);
        auto* params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        uint32_t id = 0;
        for(uint32_t i = 0, n = params->count(p); i < n; ++i)
        {
            clap_param_info_t info;
            params->get_info(p, i, &info);
            if(!std::strcmp(info.name, name)) id = info.id;
        }
        p->destroy(p);
        if(!id) { std::printf("no parameter %s\n", name); std::exit(2); }
        return id;
    }

    constexpr uint32_t F_ = SBF_FRAGMENT;

    // oscairn.sound-snapshots: a snapshot's sounds prepared beside the playing ones.
    bool prepareSnapshot(Instance& o, const std::string& snapshot, uint32_t* selected, char* message = nullptr, uint32_t capacity = 0)
    {
        struct Reader { const std::string* data; size_t pos = 0; } r{&snapshot};
        clap_istream_t in{&r, [](const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t {
            auto* r = static_cast<Reader*>(s->ctx);
            const auto n = std::min<size_t>(size, r->data->size() - r->pos);
            std::memcpy(buffer, r->data->data() + r->pos, n);
            r->pos += n;
            return int64_t(n);
        }};
        auto* x = o.ext<oscairn_plugin_sound_snapshots_t>(OSCAIRN_EXT_SOUND_SNAPSHOTS);
        return x && x->prepare(o.p, &in, selected, message, capacity);
    }
    uint32_t preparedCount(Instance& o)
    {
        auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        oscairn_sound_context_info_t info;
        uint32_t n = 0;
        for(uint32_t slot = 0; slot < c->capacity(o.p); ++slot) n += c->get(o.p, slot, &info);
        return n;
    }
    // Selects a prepared snapshot's sounds as the host does at the cue.
    void cue(Instance& o, const uint32_t* selected)
    {
        auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        if(std::all_of(selected, selected + 16, [&](uint32_t id) { return id == selected[0]; })) CHECK(c->select(o.p, selected[0], -1));
        else for(int16_t ch = 0; ch < 16; ++ch) CHECK(c->select(o.p, selected[ch], ch));
    }

    // Metadata: unique IDs, ranges, module paths, value text both ways, hints.
    void metadata()
    {
        CHECK(!std::strcmp(organ::descriptor.id, "org.oscairn.setbfree") && !std::strcmp(organ::descriptor.name, "setBfree"));
        Instance o(48000);
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        auto* ui = o.ext<oscairn_plugin_ui_t>(OSCAIRN_EXT_UI);
        CHECK(params && ui);
        std::set<clap_id> ids;
        std::set<std::string> names;
        const uint32_t n = params->count(o.p);
        CHECK(n > 90);
        uint32_t drawbarGroups = 0, roles = 0;
        for(uint32_t i = 0; i < n; ++i)
        {
            clap_param_info_t info;
            CHECK(params->get_info(o.p, i, &info));
            CHECK(ids.insert(info.id).second);
            CHECK(names.insert(info.name).second); // names are specific enough to search
            CHECK(info.min_value <= info.default_value && info.default_value <= info.max_value);
            CHECK(std::strlen(info.module) > 0);
            double value;
            CHECK(params->get_value(o.p, info.id, &value) && value == info.default_value);
            const bool stepped = info.flags & CLAP_PARAM_IS_STEPPED;
            for(double v : {info.min_value, info.default_value, info.max_value, (info.min_value + info.max_value) / 2})
            {
                if(stepped) v = std::round(v);
                char text[256];
                CHECK(params->value_to_text(o.p, info.id, v, text, sizeof(text)));
                double back;
                if(!params->text_to_value(o.p, info.id, text, &back)) { std::printf("  %s: '%s' does not parse\n", info.name, text); CHECK(false); continue; }
                // Text is rounded for display; reading it back lands on the same text.
                char again[256];
                params->value_to_text(o.p, info.id, back, again, sizeof(again));
                if(std::strcmp(text, again)) std::printf("  %s: '%s' reads back as '%s'\n", info.name, text, again);
                CHECK(!std::strcmp(text, again));
            }
            oscairn_param_hint_t hint;
            CHECK(ui->param_hint(o.p, info.id, &hint));
            if(!std::strncmp(info.module, "Upper/Drawbars", 14))
            {
                CHECK(hint.widget == OSCAIRN_WIDGET_SLIDER && hint.group == 1 && stepped);
                CHECK(hint.priority >= 1 && hint.priority <= 9);
                ++drawbarGroups;
            }
            if(hint.role) ++roles;
            if(stepped && info.max_value - info.min_value == 1 && !std::strcmp(info.name, "Percussion"))
                CHECK(hint.widget == OSCAIRN_WIDGET_SWITCH);
        }
        CHECK(drawbarGroups == 9);
        CHECK(roles == 1); // Volume is the level
    }

    // Plays a chord with a drawbar change and a Leslie change.
    std::vector<float> chordPerformance(Instance& o, uint32_t block, double seconds = 2.0)
    {
        const auto rate = o.rate;
        const uint32_t total = uint32_t(rate * seconds);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).note(CLAP_EVENT_NOTE_ON, 0, 64, 2).note(CLAP_EVENT_NOTE_ON, 0, 67, 3);
        o.at(uint32_t(rate * 0.3)).param(param("Upper 4'"), 6);
        o.at(uint32_t(rate * 0.5)).param(param("Leslie speed"), 2).param(param("Percussion"), 1);
        o.at(uint32_t(rate * 0.7)).note(CLAP_EVENT_NOTE_ON, 0, 72, 4);
        o.at(uint32_t(rate * 1.2)).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1).note(CLAP_EVENT_NOTE_OFF, 0, 64, 2);
        o.at(uint32_t(rate * 1.5)).note(CLAP_EVENT_NOTE_OFF, 0, 67, 3).note(CLAP_EVENT_NOTE_OFF, 0, 72, 4);
        return o.render(total, block).left;
    }

    // The same performance renders the same, in fresh instances and at any block size.
    void determinism()
    {
        Instance a(48000), b(48000), c(48000);
        const auto x = chordPerformance(a, 512), y = chordPerformance(b, 512), z = chordPerformance(c, 37);
        CHECK(rms(x, 0, x.size()) > 0.01);
        CHECK(x == y);
        CHECK(x == z);
    }

    // No allocation, lock or file access in process: allocations on the audio thread are counted.
    void realtime()
    {
        Instance o(48000);
        auto* contexts = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        const oscairn_sound_parameter_t bright[] = {{param("Upper 1'"), 8}};
        const auto second = contexts->prepare(o.p, "bright", bright, 1, nullptr, nullptr, nullptr, 0);
        CHECK(second != CLAP_INVALID_ID);
        resetAllocations();
        chordPerformance(o, 256, 1.0);
        // Every parameter at its maximum then minimum, live and configuration alike.
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 48, 10);
        for(uint32_t i = 0; i < params->count(o.p); ++i)
        {
            clap_param_info_t info;
            params->get_info(o.p, i, &info);
            o.at(i % 200).param(info.id, info.max_value);
        }
        o.render(4096, 512);
        o.service(); // builds the configuration changes' engine on the main thread
        o.render(48000 / 10, 512);
        for(uint32_t i = 0; i < params->count(o.p); ++i)
        {
            clap_param_info_t info;
            params->get_info(o.p, i, &info);
            o.at(i % 200).param(info.id, info.min_value);
        }
        o.render(4096, 512);
        o.service();
        o.render(48000 / 10, 512);
        // Sound switching, MIDI, chokes, expression, mod wheel, sustain.
        contexts->select(o.p, second, 0);
        o.at(10).note(CLAP_EVENT_NOTE_ON, 0, 50, 11).midi(0xB0, 1, 127).midi(0xB0, 11, 60).midi(0xB0, 64, 127).midi(0x90, 62, 100);
        o.at(300).note(CLAP_EVENT_NOTE_CHOKE, 0, 50, 11).expression(CLAP_NOTE_EXPRESSION_VOLUME, 0, 48, 10, 0.5);
        o.render(48000, 128);
        contexts->select(o.p, 0, -1);
        o.at(0).midi(0x80, 62, 0).midi(0xB0, 64, 0).midi(0xB0, 1, 0).note(CLAP_EVENT_NOTE_OFF, 0, 48, 10);
        o.render(48000 * 2, 333);
        o.service();
        std::printf("  allocations in process: %d, frees: %d\n", allocations(), frees());
        if(!allocationGuard) { std::printf("  (allocation guard off in this build)\n"); return; }
        CHECK(allocations() == 0);
        CHECK(frees() == 0);
    }

    struct Render
    {
        std::vector<float> left;
        std::vector<std::vector<uint8_t>> events;
    };

    // State: byte-identical round trips, a loaded state sounds as saved, parameters and ports
    // are the same whatever state is loaded.
    void state()
    {
        Instance a(48000);
        auto* contexts = a.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        a.at(0).param(param("Upper 2'"), 7).param(param("Tuning"), 443.5).param(param("Lower split"), 48).param(param("Volume"), -3.25);
        a.render(256, 256);
        a.service();
        a.render(256, 256);
        const oscairn_sound_parameter_t sound[] = {{param("Vibrato upper"), 1}, {param("Horn fast speed"), 380.125}};
        CHECK(contexts->prepare(a.p, "chorus \"one\"", sound, 2, nullptr, nullptr, nullptr, 0) != CLAP_INVALID_ID);
        const auto saved = a.save();
        CHECK(!saved.empty());
        Instance b(48000);
        { // loading over a sounding instance, its second slot playing
            auto* cb = b.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            const auto id = cb->prepare(b.p, "other", nullptr, 0, nullptr, nullptr, nullptr, 0);
            CHECK(cb->select(b.p, id, 0));
            b.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
            b.render(4800, 480);
        }
        CHECK(b.load(saved));
        CHECK(b.save() == saved); // the same bytes back
        CHECK(b.save() == saved); // and again, straight after
        // The saving instance has played (its Leslie has turned): reloaded, it sounds like b.
        CHECK(a.load(saved));
        const auto x = chordPerformance(a, 512), y = chordPerformance(b, 512);
        CHECK(x == y);
        auto* pa = a.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        auto* pb = b.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        CHECK(pa->count(a.p) == pb->count(b.p));
        for(uint32_t i = 0; i < pa->count(a.p); ++i)
        {
            clap_param_info_t ia, ib;
            pa->get_info(a.p, i, &ia);
            pb->get_info(b.p, i, &ib);
            CHECK(!std::memcmp(&ia, &ib, sizeof(ia)));
        }
        // A state that is not ours, or out of range, is refused and changes nothing.
        // Loaded sounds are not left pinned: once quiet, the prepared one can be discarded.
        b.render(48000 * 5, 512);
        auto* cb = b.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        oscairn_sound_context_info_t info;
        CHECK(cb->get(b.p, 0, &info) && info.idle == false); // default: selected on every channel
        CHECK(cb->get(b.p, 1, &info) && info.idle);
        const auto current = b.save();
        CHECK(!b.load("oscairn-organ-0\n"));
        std::string bad = saved;
        bad.replace(bad.find(" 443.5"), 6, " 9999");
        CHECK(!b.load(bad));
        CHECK(!b.load(saved.substr(0, saved.size() / 2)));
        CHECK(b.save() == current);
        std::printf("  state is %zu bytes\n", saved.size());

        // A refused state over a ringing tail: the tail rings on, and its pin ends with it.
        Instance c(48000);
        auto* cc = c.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        const auto other = cc->prepare(c.p, "other", nullptr, 0, nullptr, nullptr, nullptr, 0);
        CHECK(cc->select(c.p, other, 0));
        c.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).at(4800).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1);
        c.render(9600, 480);
        CHECK(cc->select(c.p, 0, 0));
        CHECK(!c.load(bad));
        const auto tail = c.render(4800, 480).left;
        CHECK(rms(tail, 0, tail.size()) > 1e-4);
        c.render(48000 * 5, 512);
        CHECK(cc->discard(c.p, other));
    }

    int noteEnds(const std::vector<std::vector<uint8_t>>& events, int32_t id = -2)
    {
        int n = 0;
        for(const auto& e : events)
        {
            auto* h = reinterpret_cast<const clap_event_header_t*>(e.data());
            if(h->type != CLAP_EVENT_NOTE_END) continue;
            auto* note = reinterpret_cast<const clap_event_note_t*>(h);
            if(id == -2 || note->note_id == id) ++n;
        }
        return n;
    }

    // Note IDs: one key pressed twice sounds until both presses end; each ends once.
    void notes()
    {
        Instance o(48000);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 100).note(CLAP_EVENT_NOTE_ON, 0, 69, 101);
        auto r = o.render(9600, 480);
        const double held = rms(r.left, 4800, 9600);
        CHECK(held > 0.01);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 69, 100);
        r = o.render(9600, 480);
        CHECK(noteEnds(r.events, 100) == 1);
        CHECK(noteEnds(r.events, 101) == 0);
        CHECK(rms(r.left, 4800, 9600) > 0.7 * held); // the second press still holds the key
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 69, 101);
        r = o.render(48000, 480);
        CHECK(noteEnds(r.events, 101) == 1);
        // A wildcard note-off ends everything on the channel.
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).note(CLAP_EVENT_NOTE_ON, 0, 62, 2).note(CLAP_EVENT_NOTE_ON, 1, 48, 3);
        o.render(480, 480);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, -1, -1);
        r = o.render(4800, 480);
        CHECK(noteEnds(r.events, 1) == 1 && noteEnds(r.events, 2) == 1 && noteEnds(r.events, 3) == 0);
        // MIDI notes too.
        o.at(0).midi(0x90, 72, 100);
        r = o.render(4800, 480);
        CHECK(rms(r.left, 2400, 4800) > 0.01);
        o.at(0).midi(0x80, 72, 0).note(CLAP_EVENT_NOTE_OFF, 1, -1, -1);
        r = o.render(48000 * 3, 480);
        CHECK(rms(r.left, r.left.size() - 4800, r.left.size()) < 1e-4);
    }

    // A host choke fades the only key of a sound in 2-5 ms; per-note volume scales it. With
    // another key of the sound held, the choked key's own release is its fade: 2-5 ms too.
    void chokeAndVolume()
    {
        for(double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        {
            Instance o(rate);
            o.at(0).param(param("Leslie"), 0).param(param("Reverb"), 0);
            o.render(256, 256);
            o.service();
            o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 7);
            auto r = o.render(uint32_t(rate / 2), 128);
            const size_t window = size_t(rate / 10);
            const double before = rms(r.left, r.left.size() - window, r.left.size());
            o.at(0).expression(CLAP_NOTE_EXPRESSION_VOLUME, 0, 69, 7, 0.5);
            r = o.render(uint32_t(rate / 4), 128);
            const double half = rms(r.left, r.left.size() - window, r.left.size());
            CHECK(std::fabs(half / before - 0.5) < 0.01);
            o.at(100).note(CLAP_EVENT_NOTE_CHOKE, 0, 69, 7);
            r = o.render(uint32_t(rate / 10), 128);
            // Silent within 5 ms, and no sample jumps by more than the fade allows.
            const size_t fiveMs = size_t(rate * 0.005);
            double tail = 0;
            for(size_t i = 100 + fiveMs; i < r.left.size(); ++i) tail = std::max(tail, std::fabs(double(r.left[i])));
            CHECK(tail < 1e-4);
            double peak = 0;
            for(size_t i = 0; i < 100; ++i) peak = std::max(peak, std::fabs(double(r.left[i])));
            double fadeStep = 0;
            const size_t twoMs = size_t(rate * 0.002);
            for(size_t i = 100; i < 100 + twoMs; ++i) fadeStep = std::max(fadeStep, std::fabs(double(r.left[i])));
            CHECK(fadeStep > 0.2 * peak); // not cut: still sounding during the first 2 ms
            CHECK(noteEnds(r.events, 7) == 1);

            // A silent lower-manual key (D#3: no tonewheel in common with A4) holds the sound up;
            // the choked key's gain is a / held.
            Instance a(rate), held(rate);
            for(auto* i : {&a, &held})
            {
                i->at(0).param(param("Leslie"), 0).param(param("Reverb"), 0);
                i->render(256, 256);
                i->service();
                i->at(0).note(CLAP_EVENT_NOTE_ON, 1, 51, 1).note(CLAP_EVENT_NOTE_ON, 0, 69, 2);
            }
            const uint32_t at = uint32_t(rate / 2) + 77;
            a.at(at).note(CLAP_EVENT_NOTE_CHOKE, 0, 69, 2);
            const auto x = a.render(at + uint32_t(rate / 50), 128).left, y = held.render(at + uint32_t(rate / 50), 128).left;
            double top = 0;
            for(size_t i = at - 4096; i < at; ++i) top = std::max(top, std::fabs(double(y[i])));
            size_t onset = at, end = 0;
            while(onset < x.size() && x[onset] == y[onset]) ++onset;
            for(size_t i = onset; i < x.size() && !end; ++i)
                if(std::fabs(double(y[i])) > 0.05 * top && std::fabs(double(x[i]) / y[i]) < 0.01) end = i;
            const double fade = (double(end) - double(onset)) / rate * 1000, total = (double(end) - at) / rate * 1000;
            std::printf("  %.0f Hz: choked with another key held, fades over %.2f ms, silent %.2f ms after the choke\n", rate, fade, total);
            CHECK(end > onset && fade >= 2 && fade <= 5 && total <= 6);
        }

        // The fade takes its 3 ms whatever the block size: a choke 100 frames into a 4096-frame
        // block renders as it does in 128-frame blocks, and is silent 5 ms on.
        std::vector<float> x[2];
        for(uint32_t block : {128u, 4096u})
        {
            Instance o(48000);
            o.at(0).param(param("Leslie"), 0).param(param("Reverb"), 0);
            o.render(256, 256);
            o.service();
            o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 7);
            o.render(24576, block);
            o.at(100).note(CLAP_EVENT_NOTE_CHOKE, 0, 69, 7);
            x[block == 4096] = o.render(8192, block).left;
        }
        double tail = 0;
        for(size_t i = 100 + 240; i < x[1].size(); ++i) tail = std::max(tail, std::fabs(double(x[1][i])));
        CHECK(x[0] == x[1] && tail < 1e-4);
    }

    // Magnitude of x[from, to) at a frequency (Hann window).
    double magnitude(const std::vector<float>& x, size_t from, size_t to, double hz, double rate)
    {
        double re = 0, im = 0;
        const double n = double(to - from);
        for(size_t i = from; i < to; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2 * M_PI * double(i - from) / n), a = 2 * M_PI * hz * double(i) / rate;
            re += w * x[i] * std::cos(a); im += w * x[i] * std::sin(a);
        }
        return std::sqrt(re * re + im * im) / n;
    }

    // The sound-context helper's voiceFinished hook fires whenever a voice ends, and its key
    // goes up: released out, choked, stolen, reset.
    void voices()
    {
        Instance o(48000);
        o.at(0).param(param("Leslie"), 0).param(param("Reverb"), 0);
        o.render(256, 256);
        o.service();
        auto finished = [&] { return organ::voicesFinished(o.p); };
        // Released.
        auto before = finished();
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).at(960).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1);
        auto r = o.render(4800, 480);
        CHECK(finished() == before + 1 && noteEnds(r.events, 1) == 1);
        // Choked, another key of the sound held (silent lower D#3): the key goes up.
        before = finished();
        o.at(0).note(CLAP_EVENT_NOTE_ON, 1, 51, 2).note(CLAP_EVENT_NOTE_ON, 0, 69, 3).at(9600).note(CLAP_EVENT_NOTE_CHOKE, 0, 69, 3);
        r = o.render(19200, 480);
        CHECK(finished() == before + 1 && noteEnds(r.events, 3) == 1);
        CHECK(rms(r.left, 14400, 19200) < 1e-4);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 1, 51, 2);
        o.render(4800, 480);
        // Stolen: 192 voices, the oldest on C4 alone, then one more; C4 stops sounding.
        before = finished();
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 10);
        for(int i = 0; i < 191; ++i) o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 64, 100 + i);
        r = o.render(9600, 480);
        CHECK(finished() == before);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 67, 400);
        const auto after = o.render(9600, 480);
        CHECK(finished() == before + 1 && noteEnds(after.events, 10) == 1);
        const double c4 = magnitude(r.left, 4800, 9600, 261.63, 48000), gone = magnitude(after.left, 4800, 9600, 261.63, 48000);
        std::printf("  stolen C4: %.5f before, %.5f after\n", c4, gone);
        CHECK(gone < 0.05 * c4);
        // Reset: every voice ends, and a key played again sounds.
        before = finished();
        o.p->reset(o.p);
        CHECK(finished() == before + 192);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 64, 500);
        r = o.render(9600, 480);
        CHECK(rms(r.left, 4800, 9600) > 0.01);
    }

    // Native sound contexts: a held key keeps its sound, eight sounds, discard rules, snapshots.
    void soundContexts()
    {
        Instance o(48000);
        auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        CHECK(c && c->capacity(o.p) == 8);
        const oscairn_sound_parameter_t flute[] = {{param("Upper 16'"), 0}, {param("Upper 5 1/3'"), 0}, {param("Upper 8'"), 0}, {param("Upper 4'"), 8}};
        const auto a = c->prepare(o.p, "flute", flute, 4, nullptr, nullptr, nullptr, 0);
        CHECK(a != CLAP_INVALID_ID);
        // Reference: the default sound alone, then the flute alone.
        Instance ref(48000);
        ref.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        const auto alone = ref.render(48000, 480).left;
        // Hold a key on the default sound, select the flute, play another key.
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        o.render(24000, 480);
        CHECK(c->select(o.p, a, 0));
        o.render(480, 480);
        oscairn_sound_context_info_t info;
        CHECK(c->get(o.p, 0, &info) && info.active_voices == 1 && !info.idle);
        CHECK(!c->discard(o.p, a)); // selected
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 72, 2);
        auto r = o.render(23520, 480);
        bool found = false;
        for(uint32_t slot = 0; slot < 8; ++slot)
            if(c->get(o.p, slot, &info) && info.id == a) { found = true; CHECK(info.active_voices == 1 && info.selected_channels == 1); }
        CHECK(found);
        // The held key still sounds like the default sound: the mix minus the flute's own key.
        (void)alone;
        // Back to default, release everything: the flute ends its tail and can be discarded.
        CHECK(c->select(o.p, 0, 0));
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, -1, -1);
        r = o.render(48000 * 4, 480);
        CHECK(c->discard(o.p, a));
        CHECK(!c->discard(o.p, a));
        CHECK(!c->discard(o.p, 0));
        // Seven more sounds fill the slots; an eighth is refused.
        std::vector<uint32_t> ids;
        for(int i = 0; i < 7; ++i)
        {
            const oscairn_sound_parameter_t v[] = {{param("Upper 8'"), double(i)}};
            ids.push_back(c->prepare(o.p, ("sound " + std::to_string(i)).c_str(), v, 1, nullptr, nullptr, nullptr, 0));
            CHECK(ids.back() != CLAP_INVALID_ID && ids.back() != a); // IDs are not reused
        }
        char message[128] = "";
        const oscairn_sound_parameter_t v[] = {{param("Upper 8'"), 1}};
        CHECK(c->prepare(o.p, "too many", v, 1, nullptr, nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        CHECK(std::strlen(message) > 0);
        // Bad values are refused with a reason.
        const oscairn_sound_parameter_t bad[] = {{param("Upper 8'"), 9}};
        CHECK(c->discard(o.p, ids[6]));
        CHECK(c->prepare(o.p, "bad", bad, 1, nullptr, nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        // Each channel plays its own sound.
        CHECK(c->select(o.p, ids[1], 1) && c->select(o.p, ids[2], 2));
        o.at(0).note(CLAP_EVENT_NOTE_ON, 1, 60, 5).note(CLAP_EVENT_NOTE_ON, 2, 60, 6);
        o.render(4800, 480);
        for(uint32_t slot = 0; slot < 8; ++slot)
            if(c->get(o.p, slot, &info) && (info.id == ids[1] || info.id == ids[2])) CHECK(info.active_voices == 1);
        // The snapshot keeps the sounds and selections.
        const auto saved = o.save();
        Instance b(48000);
        CHECK(b.load(saved));
        CHECK(b.save().size() == saved.size());
        std::printf("  snapshot with %zu sounds: %zu bytes\n", ids.size(), saved.size());
    }

    // Asleep when nothing sounds, awake while a key or a tail does.
    void sleep()
    {
        Instance o(48000);
        CHECK(o.process(512) == CLAP_PROCESS_SLEEP);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        CHECK(o.process(512) == CLAP_PROCESS_CONTINUE);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1);
        CHECK(o.process(512) == CLAP_PROCESS_CONTINUE); // the reverb and Leslie still ring
        uint32_t blocks = 0;
        while(o.process(512) == CLAP_PROCESS_CONTINUE && blocks < 48000 / 512 * 10) ++blocks;
        CHECK(blocks < 48000 / 512 * 10);
        std::printf("  tail ended after %.2f s\n", blocks * 512 / 48000.0);
    }

    // The mod wheel and the sustain pedal switch the Leslie, and say so.
    void performanceControls()
    {
        Instance o(48000);
        const auto leslie = param("Leslie speed");
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        double v;
        o.at(0).midi(0xB0, 1, 100);
        auto r = o.render(512, 512);
        CHECK(params->get_value(o.p, leslie, &v) && v == 2);
        bool reported = false;
        for(const auto& e : r.events)
        {
            auto* h = reinterpret_cast<const clap_event_header_t*>(e.data());
            if(h->type == CLAP_EVENT_PARAM_VALUE && reinterpret_cast<const clap_event_param_value_t*>(h)->param_id == leslie) reported = true;
        }
        CHECK(reported);
        o.at(0).midi(0xB0, 1, 10);
        o.render(512, 512);
        CHECK(params->get_value(o.p, leslie, &v) && v == 1);
        o.at(0).midi(0xB0, 64, 127);
        o.render(512, 512);
        CHECK(params->get_value(o.p, leslie, &v) && v == 2);
        o.at(0).midi(0xB0, 64, 0).midi(0xB0, 64, 127);
        o.render(512, 512);
        CHECK(params->get_value(o.p, leslie, &v) && v == 1);
        // In "Hold notes", the pedal holds keys instead.
        o.at(0).param(param("Sustain pedal"), 1).midi(0xB0, 64, 0).midi(0xB0, 64, 127).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        o.render(512, 512);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1);
        r = o.render(4800, 512);
        CHECK(noteEnds(r.events, 1) == 0);
        CHECK(params->get_value(o.p, leslie, &v) && v == 1);
        o.at(0).midi(0xB0, 64, 0);
        r = o.render(4800, 512);
        CHECK(noteEnds(r.events, 1) == 1);
        // Expression is the swell pedal: quieter, not silent.
        Instance loud(48000), soft(48000);
        loud.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        soft.at(0).midi(0xB0, 11, 40).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        const auto a = loud.render(24000, 480).left, b = soft.render(24000, 480).left;
        const double ra = rms(a, 12000, 24000), rb = rms(b, 12000, 24000);
        CHECK(rb < ra * 0.5 && rb > ra * 0.05);
    }

    // A configuration change while a key is held crossfades to the new engine, the key still down.
    void rebuildWhilePlaying()
    {
        Instance o(48000);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
        o.render(24000, 480);
        o.at(0).param(param("Tuning"), 466.16); // a semitone up
        o.render(480, 480);
        CHECK(host.callbacks > 0);
        o.service();
        auto r = o.render(24000, 480);
        CHECK(rms(r.left, 12000, 24000) > 0.01); // the key is still down in the new engine
        double jump = 0;
        for(size_t i = 1; i < 2000; ++i) jump = std::max(jump, std::fabs(double(r.left[i]) - r.left[i - 1]));
        std::printf("  largest step across the crossfade: %.4f\n", jump);
        o.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 69, 1);
        r = o.render(4800, 480);
        CHECK(noteEnds(r.events, 1) == 1);
    }

    // The main thread prepares, selects, discards and builds engines while the audio thread plays
    // (run under ThreadSanitizer in CI).
    void threads()
    {
        const auto snapshot = Instance(48000).save(); // one sound, prepared beside while rendering
        Instance o(48000);
        auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        const auto tuning = param("Tuning"), drawbar = param("Upper 8'");
        std::atomic<bool> stop{false}, play{true};
        std::atomic<uint32_t> blocks{0};
        std::thread audio([&] {
            bool released = false;
            for(uint32_t i = 0; !stop; ++i)
            {
                if(play)
                {
                    o.at(0).note(i % 2 ? CLAP_EVENT_NOTE_OFF : CLAP_EVENT_NOTE_ON, int16_t(i % 3), int16_t(48 + i % 24), int32_t(i / 2));
                    if(i % 200 == 0) o.at(3).param(tuning, 430 + i % 20);
                    if(i % 7 == 0) o.at(9).param(drawbar, i % 9);
                }
                else if(!released)
                {
                    released = true;
                    for(int16_t ch = 0; ch < 3; ++ch) o.at(0).note(CLAP_EVENT_NOTE_OFF, ch, -1, -1);
                }
                o.process(256);
                ++blocks;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
        uint32_t prepared = 0, discarded = 0, beside = 0;
        std::vector<uint32_t> ids;
        for(int round = 0; round < 300; ++round)
        {
            if(round == 200) play = false; // then the sounds' tails end and they can go
            o.service();
            if(ids.size() < 4)
            {
                const oscairn_sound_parameter_t v[] = {{drawbar, double(round % 9)}};
                const auto id = c->prepare(o.p, "t", v, 1, nullptr, nullptr, nullptr, 0);
                if(id != CLAP_INVALID_ID) { ids.push_back(id); ++prepared; }
                uint32_t selected[16];
                if(round % 10 == 0 && prepareSnapshot(o, snapshot, selected)) { ids.push_back(selected[0]); ++prepared; ++beside; }
            }
            if(!ids.empty())
            {
                c->select(o.p, ids[round % ids.size()], int16_t(round % 3));
                if(round % 4 == 3) { c->select(o.p, 0, -1); if(c->discard(o.p, ids.front())) { ids.erase(ids.begin()); ++discarded; } }
            }
            oscairn_sound_context_info_t info;
            for(uint32_t slot = 0; slot < 8; ++slot) c->get(o.p, slot, &info);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for(int wait = 0; !ids.empty() && wait < 5000; ++wait)
        {
            o.service();
            c->select(o.p, 0, -1);
            if(c->discard(o.p, ids.back())) { ids.pop_back(); ++discarded; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stop = true;
        audio.join();
        o.service();
        std::printf("  %u blocks, %u prepared (%u from a snapshot), %u discarded\n", blocks.load(), prepared, beside, discarded);
        CHECK(blocks > 100 && prepared >= 4 && beside >= 1 && discarded == prepared);
    }

    // Nothing on stdout or stderr from building engines or playing (it is the host's).
    void quiet()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        FILE* sink = std::tmpfile();
        const int out = dup(1), err = dup(2);
        dup2(fileno(sink), 1);
        dup2(fileno(sink), 2);
        {
            Instance o(48000);
            auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
            o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
            for(double which : {1.0, 0.0})
                for(uint32_t i = 0; i < params->count(o.p); ++i)
                {
                    clap_param_info_t info;
                    params->get_info(o.p, i, &info);
                    o.at(i).param(info.id, which ? info.max_value : info.min_value);
                }
            o.render(4800, 480);
            o.service();
            o.render(4800, 480);
        }
        std::fflush(stdout);
        std::fflush(stderr);
        dup2(out, 1);
        dup2(err, 2);
        close(out);
        close(err);
        const long written = std::ftell(sink);
        std::rewind(sink);
        char text[200] = "";
        text[std::fread(text, 1, sizeof(text) - 1, sink)] = 0;
        std::fclose(sink);
        if(written) std::printf("  printed %ld bytes: %s\n", written, text);
        CHECK(written == 0);
    }

    // One clean 8' tone: no Leslie, reverb, vibrato, percussion or click.
    void plain(Instance& o)
    {
        o.at(0).param(param("Upper 16'"), 0).param(param("Upper 5 1/3'"), 0).param(param("Upper 8'"), 8)
            .param(param("Leslie"), 0).param(param("Reverb"), 0).param(param("Key click"), 0).param(param("Release click"), 0);
        o.render(256, 256);
        o.service();
        o.render(256, 256);
    }

    // Envelope (RMS over 10 ms windows) of a signal.
    std::vector<double> envelope(const std::vector<float>& x, double rate)
    {
        const size_t w = size_t(rate / 100);
        std::vector<double> e;
        for(size_t i = 0; i + w <= x.size(); i += w) e.push_back(rms(x, i, i + w));
        return e;
    }

    struct RateMeasure { double pitch, vibrato, percussion, leslie, horn; };
    RateMeasure measure(double rate)
    {
        RateMeasure m{};
        { // tonewheel pitch of A4
            Instance o(rate);
            plain(o);
            o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
            const auto x = o.render(uint32_t(rate * 2), 512).left;
            m.pitch = frequency(x, size_t(rate * 0.5), x.size(), rate);
        }
        { // vibrato V3: the spread of the period-by-period pitch
            Instance o(rate);
            plain(o);
            o.at(0).param(param("Vibrato"), 4).param(param("Vibrato upper"), 1).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
            const auto x = o.render(uint32_t(rate * 2), 512).left;
            double lo = 1e9, hi = 0;
            const size_t step = size_t(rate / 200); // 5 ms windows
            for(size_t i = size_t(rate * 0.5); i + 2 * step < x.size(); i += step)
            {
                const double f = frequency(x, i, i + 2 * step, rate);
                if(f > 0) { lo = std::min(lo, f); hi = std::max(hi, f); }
            }
            m.vibrato = 1200 * std::log2(hi / lo); // cents peak to peak
        }
        { // fast percussion: the difference it makes to an 8' tone, time to fall 20 dB
            Instance o(rate), q(rate);
            plain(o);
            plain(q);
            o.at(0).param(param("Percussion"), 1).param(param("Percussion decay"), 1).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
            q.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
            auto x = o.render(uint32_t(rate * 2), 512).left;
            const auto y = q.render(uint32_t(rate * 2), 512).left;
            for(size_t i = 0; i < x.size(); ++i) x[i] -= y[i];
            const auto e = envelope(x, rate);
            const auto peak = std::max_element(e.begin(), e.end());
            auto fall = peak;
            while(fall != e.end() && *fall > *peak * 0.1) ++fall;
            m.percussion = double(fall - peak) * 0.01;
        }
        { // Leslie horn fast: the amplitude modulation's rate after spin-up
            Instance o(rate);
            o.at(0).param(param("Upper 16'"), 0).param(param("Upper 5 1/3'"), 0).param(param("Upper 8'"), 8)
                .param(param("Reverb"), 0).param(param("Leslie speed"), 2).note(CLAP_EVENT_NOTE_ON, 0, 93, 1);
            const auto x = o.render(uint32_t(rate * 6), 512).left;
            m.leslie = rotorRate(x, size_t(rate * 3), rate) * 60; // RPM
        }
        { // Leslie horn stopped: a 2960 Hz tone (4' on F#6) through it, against the Leslie off
            double level[2];
            for(int on : {0, 1})
            {
                Instance o(rate);
                plain(o);
                o.at(0).param(param("Upper 8'"), 0).param(param("Upper 4'"), 8).param(param("Leslie"), on).param(param("Leslie speed"), 0);
                o.render(256, 256);
                o.service();
                o.render(256, 256);
                o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 90, 1);
                const auto x = o.render(uint32_t(rate), 512).left;
                level[on] = rms(x, size_t(rate / 2), x.size());
            }
            m.horn = level[1] / level[0];
        }
        return m;
    }

    // One key's attack and release gain, from the engine (8' only, no leakage: one tonewheel,
    // A4, so the gain is its envelope). setBfree runs a wheel only while a key uses it, so the
    // attack is measured on a wheel already turning for a lower-manual key (16', also A4), a
    // silent pedal key letting go as the key goes down so that as many keys stay down: gain is
    // (attacked - lower) / (both - lower). The release is the key over the key held. NaN where
    // the divisor is too small.
    struct KeyEnvelope { double rate; std::vector<double> attack, release; };
    KeyEnvelope keyEnvelope(double rate, const std::string& extra = "")
    {
        const std::string config = "whirl.bypass=1\nosc.compartment-crosstalk=0\nosc.transformer-crosstalk=0\n"
                                   "osc.terminalstrip-crosstalk=0\nosc.wiring-crosstalk=0\n" + extra;
        enum { Attacked, Both, Lower, Released, Held, Engines };
        sbf_engine* e[Engines];
        auto key = [&](int which, int status, int k) { const uint8_t m[3] = {uint8_t(status), uint8_t(k), uint8_t(status & 0x10 ? 127 : 0)}; sbf_midi(e[which], m, 3); };
        for(auto& x : e)
        {
            x = sbf_create(rate, config.c_str());
            for(const char* manual : {"upper", "lower", "pedal"}) // every drawbar in (MIDI 127) ...
                for(const char* footage : {"16", "513", "8", "4", "223", "2", "135", "113", "1"})
                    sbf_control(x, sbf_control_id((std::string(manual) + ".drawbar" + footage).c_str()), 127);
            for(auto [name, v] : {std::pair{"upper.drawbar8", 0}, {"lower.drawbar16", 64}, {"reverb.mix", 0}}) // ... but these
                sbf_control(x, sbf_control_id(name), uint8_t(v));
        }
        for(int w : {Attacked, Both, Lower}) key(w, 0x91, 81);
        for(int w : {Attacked, Lower}) key(w, 0x92, 36);
        for(int w : {Both, Released, Held}) key(w, 0x90, 69);
        const uint32_t on = uint32_t(rate * 0.1) / F_ + 1, end = on + uint32_t(rate * 0.02) / F_;
        std::vector<float> out[Engines];
        float l[F_], r[F_];
        for(uint32_t f = 0; f < end; ++f)
        {
            if(f == on) { key(Attacked, 0x90, 69); key(Attacked, 0x82, 36); key(Released, 0x80, 69); }
            for(int w = 0; w < Engines; ++w) { sbf_render(e[w], l, r); out[w].insert(out[w].end(), l, l + F_); }
        }
        for(auto* x : e) sbf_destroy(x);
        double top = 0;
        for(size_t i = F_ * on / 2; i < F_ * on; ++i) top = std::max(top, std::fabs(double(out[Held][i])));
        KeyEnvelope k{rate, {}, {}};
        for(size_t i = F_ * on; i < F_ * on + size_t(rate * 0.006); ++i)
        {
            const double d = double(out[Both][i]) - out[Lower][i];
            k.attack.push_back(std::fabs(d) > 0.05 * top ? (double(out[Attacked][i]) - out[Lower][i]) / d : NAN);
            k.release.push_back(std::fabs(double(out[Held][i])) > 0.05 * top ? double(out[Released][i]) / out[Held][i] : NAN);
        }
        return k;
    }
    // ms until an attack's gain stays within 1 % of 1, and until a release's falls below 1 %.
    double attackTime(const KeyEnvelope& k)
    {
        size_t last = 0;
        for(size_t i = 0; i < k.attack.size(); ++i) if(std::fabs(k.attack[i] - 1) > 0.01) last = i + 1; // NaN compares false
        return last / k.rate * 1000;
    }
    double releaseTime(const KeyEnvelope& k)
    {
        for(size_t i = 0; i < k.release.size(); ++i) if(std::fabs(k.release[i]) < 0.01) return i / k.rate * 1000;
        return 1e9;
    }
    // Largest difference from the 44.1 kHz envelope, read between its samples.
    double shapeDifference(const std::vector<double>& g, double rate, const std::vector<double>& ref)
    {
        double worst = 0;
        for(size_t i = 0; i < g.size(); ++i)
        {
            const double t = i * 44100.0 / rate;
            const size_t k = size_t(t);
            if(std::isnan(g[i]) || k + 1 >= ref.size() || std::isnan(ref[k]) || std::isnan(ref[k + 1])) continue;
            worst = std::max(worst, std::fabs(g[i] - (ref[k] + (t - k) * (ref[k + 1] - ref[k]))));
        }
        return worst;
    }

    // A key's attack (with its click) and release take the same time, with the same shape, at
    // every rate. Upstream's took one 128-sample fragment (printed for comparison).
    void clickRates()
    {
        const auto ref = keyEnvelope(44100);
        for(double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        {
            const auto k = keyEnvelope(rate), old = keyEnvelope(rate, "osc.envelope.rate=0\n");
            const double attack = attackTime(k), release = releaseTime(k);
            const double da = shapeDifference(k.attack, rate, ref.attack), dr = shapeDifference(k.release, rate, ref.release);
            std::printf("  %.0f Hz: attack %.3f ms, release %.3f ms, differs from 44.1 kHz's by %.5f / %.5f (one fragment: %.3f, %.3f ms)\n",
                        rate, attack, release, da, dr, attackTime(old), releaseTime(old));
            CHECK(std::fabs(attack - attackTime(ref)) < 0.05 && attack > 0.3);
            CHECK(std::fabs(release - releaseTime(ref)) < 0.05 && release > 2);
            CHECK(da < 0.01 && dr < 0.01);
        }
        // A key let go during its attack and pressed again during its release, at 192 kHz where
        // they take five fragments: the gain carries on from where it is, without a step.
        Instance o(192000);
        o.at(0).param(param("Attack model"), 1).param(param("Leslie"), 0).param(param("Reverb"), 0)
            .param(param("Upper 16'"), 0).param(param("Upper 5 1/3'"), 0).param(param("Upper 8'"), 8);
        o.render(256, 256);
        o.service();
        o.render(256, 256);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 1);
        const auto steady = o.render(19200, 128).left;
        auto largestStep = [](const std::vector<float>& x, size_t from) {
            double step = 0;
            for(size_t i = from + 1; i < x.size(); ++i) step = std::max(step, std::fabs(double(x[i]) - x[i - 1]));
            return step;
        };
        uint32_t t = 0;
        for(int i = 0; i < 8; ++i, t += 150) o.at(t).note(i % 2 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, 0, 69, i % 2 ? 2 + i : -1);
        const auto overlapped = o.render(4800, 128).left;
        std::printf("  192 kHz, attacks and releases cut short: largest step %.5f, held %.5f\n", largestStep(overlapped, 0), largestStep(steady, 9600));
        CHECK(largestStep(overlapped, 0) < 1.2 * largestStep(steady, 9600));
    }

    // Transposes reach ±127 semitones: C4 played 60 semitones away sounds as C4; at the extremes
    // every key falls off the manuals and is silent (and the engine stays in its tables).
    void transpose()
    {
        auto play = [](std::initializer_list<std::pair<const char*, double>> settings, int16_t channel, int16_t key) {
            Instance o(48000);
            plain(o);
            for(auto [name, v] : settings) o.at(0).param(param(name), v);
            o.render(256, 256);
            o.service();
            o.render(256, 256);
            o.at(0).note(CLAP_EVENT_NOTE_ON, channel, key, 1);
            return o.render(24000, 512).left;
        };
        const auto c4 = play({}, 0, 60);
        const double f = frequency(c4, 12000, 24000, 48000);
        for(auto [shift, key] : {std::pair{60.0, int16_t(0)}, {-60.0, int16_t(120)}})
        {
            const auto x = play({{"Transpose", shift}}, 0, key);
            CHECK(std::fabs(1200 * std::log2(frequency(x, 12000, 24000, 48000) / f)) < 1);
        }
        for(double shift : {127.0, -127.0})
            for(int16_t key : {0, 60, 127})
            {
                CHECK(rms(play({{"Transpose", shift}}, 0, key), 0, 24000) < 1e-6);
                // Splits with their own transposes adding up to ±254.
                const auto settings = {std::pair{"Transpose", shift}, {"Pedal split", 48.0}, {"Lower split", 72.0},
                                       {"Pedal split transpose", shift}, {"Lower split transpose", shift}, {"Upper split transpose", shift}};
                CHECK(rms(play(settings, 0, key), 0, 24000) < 1e-6);
                CHECK(rms(play(settings, 1, key), 0, 24000) < 1e-6);
                CHECK(rms(play(settings, 2, key), 0, 24000) < 1e-6);
            }
        double v;
        auto* p = organ::create(&host.host);
        p->init(p);
        auto* params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        CHECK(params->text_to_value(p, param("Transpose"), "-127 semitones", &v) && v == -127);
        clap_param_info_t info;
        params->get_info(p, param("Upper split transpose") - 1, &info);
        CHECK(info.min_value == -127 && info.max_value == 127 && info.default_value == 0);
        p->destroy(p);
    }

    // Every sample rate sounds the same: pitch, vibrato, percussion decay, Leslie speed, and the
    // horn's highs as at 44.1 kHz (whirl.reference.rate).
    void rates()
    {
        const auto ref = measure(48000);
        std::printf("  48000 Hz: A4 %.4f Hz, V3 %.1f cents p-p, fast percussion -20 dB in %.2f s, horn %.1f RPM, stopped horn at 2960 Hz %.4f\n",
                    ref.pitch, ref.vibrato, ref.percussion, ref.leslie, ref.horn);
        double horn44 = 0;
        for(double rate : {44100.0, 88200.0, 96000.0, 176400.0, 192000.0})
        {
            const auto m = measure(rate);
            const double cents = 1200 * std::log2(m.pitch / ref.pitch);
            if(rate == 44100) horn44 = m.horn;
            std::printf("  %.0f Hz: A4 %+.3f cents, V3 %.1f cents p-p, percussion %.2f s, horn %.1f RPM, stopped horn at 2960 Hz %.4f (%+.2f dB)\n",
                        rate, cents, m.vibrato, m.percussion, m.leslie, m.horn, 20 * std::log10(m.horn / horn44));
            CHECK(std::fabs(cents) < 0.5);
            CHECK(std::fabs(m.vibrato - ref.vibrato) < 0.1 * ref.vibrato);
            CHECK(std::fabs(m.percussion - ref.percussion) <= 0.02);
            CHECK(std::fabs(m.leslie - ref.leslie) < 0.02 * ref.leslie);
            CHECK(std::fabs(20 * std::log10(m.horn / horn44)) < 0.75);
        }
        CHECK(std::fabs(20 * std::log10(ref.horn / horn44)) < 0.75);
    }

    // The first note of a new engine is no louder than the same note later.
    void startup()
    {
        Instance o(48000);
        o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1);
        const auto x = o.render(48000, 512).left;
        double first = 0, later = 0;
        for(size_t i = 0; i < 960; ++i) first = std::max(first, std::fabs(double(x[i])));
        for(size_t i = 9600; i < x.size(); ++i) later = std::max(later, std::fabs(double(x[i])));
        std::printf("  peak in the first 20 ms %.3f, after 0.2 s %.3f\n", first, later);
        CHECK(first < 1.3 * later);
    }

    // oscairn.memory: the estimate before activating covers what activating allocates, within 25 %.
    void memory()
    {
        if(!allocationGuard) { std::printf("  (needs the allocation guard's allocator)\n"); return; }
        for(double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        {
            Instance o(rate, false);
            auto* mem = o.ext<oscairn_plugin_memory_t>(OSCAIRN_EXT_MEMORY);
            auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            CHECK(mem && mem->estimate && mem->current);
            c->prepare(o.p, "second", nullptr, 0, nullptr, nullptr, nullptr, 0); // built at activation
            oscairn_memory_t e{};
            CHECK(mem->estimate(o.p, rate, 4096, &e));
            trackBytes(true);
            o.p->activate(o.p, rate, 1, 4096);
            trackBytes(false);
            const double estimate = double(e.shared_bytes + e.context_bytes + e.asset_bytes), held = double(trackedBytes());
            std::printf("  %.0f Hz: two sounds, estimate %.2f MB, allocated %.2f MB\n", rate, estimate / 1e6, held / 1e6);
            CHECK(estimate >= held && estimate <= 1.25 * held);
            // A third sound costs about one context.
            trackBytes(true);
            c->prepare(o.p, "third", nullptr, 0, nullptr, nullptr, nullptr, 0);
            trackBytes(false);
            CHECK(double(e.context_bytes) >= double(trackedBytes()) && double(e.context_bytes) <= 1.25 * double(trackedBytes()));
            oscairn_memory_t now{};
            CHECK(mem->current(o.p, &now) && now.shared_bytes + now.context_bytes >= e.shared_bytes + e.context_bytes);
            o.p->start_processing(o.p);
        }
    }

    // Presets: setBfree's programs are listed through preset discovery and load their drawbars.
    void presets()
    {
        auto* factory = static_cast<const clap_preset_discovery_factory_t*>(organ::factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
        CHECK(factory && factory->count(factory) == 1);
        struct Found { std::vector<std::pair<std::string, std::string>> presets; int locations = 0; std::string pluginId; } found;
        clap_preset_discovery_indexer_t indexer{CLAP_VERSION_INIT, "test", "", "", "", &found, nullptr,
            [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* l) {
                static_cast<Found*>(i->indexer_data)->locations += l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
                return true;
            }, nullptr, nullptr};
        auto* provider = factory->create(factory, &indexer, factory->get_descriptor(factory, 0)->id);
        CHECK(provider && provider->init(provider) && found.locations == 1);
        clap_preset_discovery_metadata_receiver_t receiver{};
        receiver.receiver_data = &found;
        receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
            static_cast<Found*>(r->receiver_data)->presets.emplace_back(name, key); return true; };
        receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
            static_cast<Found*>(r->receiver_data)->pluginId = id->id; };
        receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
        CHECK(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver));
        provider->destroy(provider);
        std::printf("  %zu presets, the first \"%s\"\n", found.presets.size(), found.presets.empty() ? "" : found.presets[0].first.c_str());
        CHECK(found.presets.size() == 55 && found.pluginId == "org.oscairn.setbfree");
        // Load each one, active and inactive: its drawbars become the parameters.
        for(bool activate : {true, false})
            for(const auto& [name, key] : found.presets)
            {
                Instance o(48000, activate);
                auto* load = o.ext<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD);
                CHECK(load->from_location(o.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str()));
                if(activate) o.render(512, 512);
                sbf_program g;
                CHECK(sbf_program_get(std::stoi(key), &g));
                auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
                const auto first = param("Upper 16'");
                for(int i = 0; i < 9; ++i)
                {
                    double v;
                    params->get_value(o.p, first + i, &v);
                    if(v != g.upper[i]) { std::printf("  %s: upper drawbar %d is %g, not %d\n", name.c_str(), i, v, g.upper[i]); CHECK(false); }
                }
            }
        Instance o(48000);
        CHECK(!o.ext<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD)->from_location(o.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "200"));

        // A sound prepared from a preset (oscairn.sound-contexts draft 5) sounds as the program
        // loaded with preset-load; its saved Sound holds the values, not the preset's key, so it
        // opens whatever the preset list becomes.
        auto* contexts = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        CHECK(contexts->prepare_preset);
        char message[128] = "";
        CHECK(contexts->prepare_preset(o.p, "x", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "26", message, sizeof(message)) == CLAP_INVALID_ID && *message);
        CHECK(contexts->prepare_preset(o.p, "x", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, "somewhere", "0", nullptr, 0) == CLAP_INVALID_ID);
        CHECK(contexts->prepare_preset(o.p, "x", CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere.pgm", "0", nullptr, 0) == CLAP_INVALID_ID);
        auto chord = [](Instance& i) {
            i.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).note(CLAP_EVENT_NOTE_ON, 0, 64, 2).note(CLAP_EVENT_NOTE_ON, 0, 67, 3);
            return i.render(9600, 512).left;
        };
        for(const auto& [name, key] : found.presets)
        {
            Instance prepared(48000), loaded(48000);
            auto* c = prepared.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            const auto id = c->prepare_preset(prepared.p, name.c_str(), CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str(), message, sizeof(message));
            CHECK(id != CLAP_INVALID_ID && c->select(prepared.p, id, -1));
            CHECK(loaded.ext<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD)->from_location(loaded.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str()));
            for(auto* i : {&prepared, &loaded}) { i->render(512, 512); i->service(); }
            const auto a = chord(prepared), b = chord(loaded);
            if(a != b) std::printf("  %s (%s): prepared from the preset, it sounds unlike the program loaded\n", name.c_str(), key.c_str());
            CHECK(a == b && (rms(a, 4800, 9600) > 1e-3 || name == "Silence")); // program 21 is silent
            // Reopened from its state (patch values only; no preset is looked up), it sounds the same.
            const auto saved = prepared.save();
            Instance reopened(48000), again(48000);
            CHECK(reopened.load(saved) && reopened.save() == saved && again.load(saved));
            for(auto* i : {&reopened, &again}) { i->render(512, 512); i->service(); }
            CHECK(chord(reopened) == chord(again));
        }
        std::printf("  %zu sounds prepared from presets, each as its program\n", found.presets.size());
    }

    void snapshots()
    {
        auto* x = Instance(48000).ext<oscairn_plugin_sound_snapshots_t>(OSCAIRN_EXT_SOUND_SNAPSHOTS);
        CHECK(x && x->prepare);
        if(!x) return;
        // S: a flute on the Default (channel 1 and up) and square wheels with percussion on channel 2.
        std::string S;
        {
            Instance s(48000);
            auto* c = s.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            s.at(0).param(param("Upper 16'"), 0).param(param("Upper 4'"), 8);
            s.render(512, 512);
            const oscairn_sound_parameter_t square[] = {{param("Wheel shape"), 2}, {param("Percussion"), 1}};
            CHECK(c->select(s.p, c->prepare(s.p, "square", square, 2, nullptr, nullptr, nullptr, 0), 1));
            s.render(512, 512);
            S = s.save();
        }
        auto play = [](Instance& i, uint32_t frames) {
            i.at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).note(CLAP_EVENT_NOTE_ON, 1, 67, 2);
            i.at(9600).note(CLAP_EVENT_NOTE_OFF, 0, 60, 1);
            return i.render(frames, 480);
        };

        // Prepared beside the default sound and selected, S plays as S loaded.
        Instance loaded(48000), beside(48000);
        CHECK(loaded.load(S));
        loaded.render(512, 512);
        beside.render(512, 512);
        uint32_t selected[16];
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(prepareSnapshot(beside, S, selected));
        const double prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(preparedCount(beside) == 3);
        CHECK(selected[0] != 0 && selected[1] != 0 && selected[0] != selected[1]);
        for(int ch = 2; ch < 16; ++ch) CHECK(selected[ch] == selected[0]);
        auto* c = beside.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        oscairn_sound_context_info_t info;
        for(uint32_t slot = 1; slot < 3; ++slot) CHECK(c->get(beside.p, slot, &info) && info.selected_channels == 0); // nothing selected yet
        cue(beside, selected);
        const auto t1 = std::chrono::steady_clock::now();
        beside.process(480);
        const double cueMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        loaded.render(480, 480);
        const auto a = play(loaded, 48000), b = play(beside, 48000);
        CHECK(a.left == b.left && a.right == b.right && rms(a.left, 0, 48000) > 1e-3);
        std::printf("  2 sounds prepared beside in %.1f ms; the cue's block (480 frames) took %.2f ms\n", prepareMs, cueMs);

        // A key held on the outgoing sound rings out beside the incoming one, which plays as alone.
        Instance mixed(48000), old(48000), fresh(48000);
        CHECK(fresh.load(S));
        const oscairn_sound_parameter_t bright[] = {{param("Upper 4'"), 8}, {param("Upper 2'"), 8}};
        uint32_t outgoing = 0;
        for(auto* i : {&mixed, &old})
        {
            auto* ic = i->ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            outgoing = ic->prepare(i->p, "bright", bright, 2, nullptr, nullptr, nullptr, 0);
            CHECK(outgoing != CLAP_INVALID_ID && ic->select(i->p, outgoing, -1));
            i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 48, 9);
            i->render(4800, 480);
        }
        fresh.render(4800, 480);
        CHECK(prepareSnapshot(mixed, S, selected));
        cue(mixed, selected);
        for(auto* i : {&mixed, &old, &fresh}) i->render(480, 480);
        for(auto* i : {&mixed, &fresh}) i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 72, 10);
        for(auto* i : {&mixed, &old}) i->at(4800).note(CLAP_EVENT_NOTE_OFF, 0, 48, 9);
        auto* m = mixed.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        CHECK(!m->discard(mixed.p, selected[0]) && !m->discard(mixed.p, selected[1])); // selected
        CHECK(!m->discard(mixed.p, outgoing)); // its key is down
        const auto y = mixed.render(2400, 480).left, o1 = old.render(2400, 480).left, f1 = fresh.render(2400, 480).left;
        bool sum = true;
        for(size_t i = 0; i < y.size(); ++i) sum &= y[i] == o1[i] + f1[i];
        CHECK(sum && rms(o1, 0, 2400) > 1e-3 && rms(f1, 0, 2400) > 1e-3);
        auto outgoingInfo = [&] { for(uint32_t slot = 0; slot < 8; ++slot) if(m->get(mixed.p, slot, &info) && info.id == outgoing) return true; return false; };
        CHECK(outgoingInfo() && info.active_voices == 1 && !info.idle);
        // The outgoing sound is retired only once its key and tail have ended.
        mixed.render(4800, 480);
        CHECK(outgoingInfo() && info.active_voices == 0 && !info.idle); // its tail rings
        CHECK(!m->discard(mixed.p, outgoing));
        uint32_t frames = 0;
        while(!(outgoingInfo() && info.idle) && frames < 48000 * 10) frames += 4800, mixed.render(4800, 480);
        std::printf("  the outgoing sound's tail ended %.1f s after its key\n", (frames + 4800 + 2400) / 48000.0);
        CHECK(info.idle && m->discard(mixed.p, outgoing));
        CHECK(!m->discard(mixed.p, 0)); // the Default is never discarded

        // With a ring-out buffer (draft 3) the retiring sound's key and tail go there, not to the
        // main output: the main output is the incoming sound alone, the buffer the outgoing alone.
        {
            Instance mixed(48000), old(48000), fresh(48000);
            Output ring;
            mixed.ringOut = &ring;
            CHECK(fresh.load(S));
            uint32_t outgoing = 0;
            for(auto* i : {&mixed, &old})
            {
                auto* ic = i->ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
                outgoing = ic->prepare(i->p, "bright", bright, 2, nullptr, nullptr, nullptr, 0);
                CHECK(outgoing != CLAP_INVALID_ID && ic->select(i->p, outgoing, -1));
                i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 48, 9);
            }
            const auto before = mixed.render(4800, 480);
            CHECK(before.left == old.render(4800, 480).left && rms(before.left, 0, 4800) > 1e-3);
            CHECK(rms(ring.left, 0, ring.left.size()) == 0); // selected: not retiring
            fresh.render(4800, 480);
            CHECK(prepareSnapshot(mixed, S, selected));
            cue(mixed, selected);
            ring = {};
            mixed.at(0).note(CLAP_EVENT_NOTE_ON, 0, 72, 10);
            fresh.at(0).note(CLAP_EVENT_NOTE_ON, 0, 72, 10);
            for(auto* i : {&mixed, &old}) i->at(2400).note(CLAP_EVENT_NOTE_OFF, 0, 48, 9);
            const auto main = mixed.render(48000, 480), alone = fresh.render(48000, 480), out = old.render(48000, 480);
            CHECK(main.left == alone.left && main.right == alone.right && rms(alone.left, 0, 48000) > 1e-3);
            CHECK(ring.left == out.left && ring.right == out.right && rms(out.left, 24000, 48000) > 1e-5); // the tail, after the key
            std::printf("  ring-out: the outgoing tail at %.2g rms in the buffer, none in the main output\n", rms(ring.left, 24000, 48000));
        }

        // A second change while the first's sound still rings out: choke_retiring fades that sound,
        // key and tail, into channels 2 and 3 over the choke's 3 ms, and it reports idle once faded;
        // the sound the second change retires takes channels 0 and 1, the incoming one the main output.
        {
            std::string T; // one sound, a 2' registration on the Default
            {
                Instance t(48000);
                t.at(0).param(param("Upper 16'"), 0).param(param("Upper 2'"), 8);
                t.render(512, 512);
                T = t.save();
            }
            Instance mixed(48000), old(48000), sAlone(48000), tAlone(48000);
            Output ring, choked;
            mixed.ringOut = &ring;
            mixed.chokeOut = &choked;
            CHECK(sAlone.load(S) && tAlone.load(T));
            uint32_t outgoing = 0;
            for(auto* i : {&mixed, &old})
            {
                auto* ic = i->ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
                outgoing = ic->prepare(i->p, "bright", bright, 2, nullptr, nullptr, nullptr, 0);
                CHECK(outgoing != CLAP_INVALID_ID && ic->select(i->p, outgoing, -1));
                i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 48, 9);
                i->render(4800, 480);
            }
            CHECK(prepareSnapshot(mixed, S, selected)); // the first change, a key held on its sound too
            cue(mixed, selected);
            for(auto* i : {&mixed, &sAlone}) i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 72, 10);
            for(auto* i : {&mixed, &old, &sAlone}) i->render(4800, 480);
            uint32_t selected2[16];
            CHECK(prepareSnapshot(mixed, T, selected2)); // the second change
            CHECK(x->choke_retiring(mixed.p));
            cue(mixed, selected2);
            ring = {}; choked = {};
            for(auto* i : {&mixed, &tAlone}) i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 76, 11);
            old.at(0).note(CLAP_EVENT_NOTE_OFF, 0, 48, 9); // a choked key lets go at once: the same engine output, before the fade
            auto* m = mixed.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
            auto brightInfo = [&] { for(uint32_t slot = 0; slot < 8; ++slot) if(m->get(mixed.p, slot, &info) && info.id == outgoing) return true; return false; };
            Output main;
            for(uint32_t block = 0; block < 100; ++block)
            {
                mixed.process(480, &main);
                if(block == 1) CHECK(brightInfo() && info.idle); // faded: idle at once
            }
            const auto t = tAlone.render(48000, 480), s = sAlone.render(48000, 480), b = old.render(48000, 480);
            CHECK(main.left == t.left && main.right == t.right && rms(t.left, 0, 48000) > 1e-3);
            CHECK(ring.left == s.left && ring.right == s.right && rms(s.left, 0, 48000) > 1e-3);
            const uint32_t fade = 144; // 3 ms
            double err = 0, after = 0;
            for(uint32_t k = 0; k < 48000; ++k)
                if(k < fade) err = std::max(err, std::fabs(double(choked.left[k] - b.left[k] * float(fade - k) / float(fade))));
                else after = std::max(after, std::fabs(double(choked.left[k])));
            CHECK(err < 1e-6 && after == 0 && rms(b.left, 0, fade) > 1e-3);
            CHECK(brightInfo() && info.idle && m->discard(mixed.p, outgoing));
        }

        // All or nothing: a bad snapshot, or too few free contexts, adds nothing and says why.
        char message[128] = "";
        Instance full(48000);
        CHECK(!prepareSnapshot(full, "oscairn-organ-1 nonsense", selected, message, sizeof(message)) && *message && preparedCount(full) == 1);
        auto* f = full.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        for(int i = 0; i < 6; ++i) CHECK(f->prepare(full.p, "filler", nullptr, 0, nullptr, nullptr, nullptr, 0) != CLAP_INVALID_ID);
        *message = 0;
        CHECK(!prepareSnapshot(full, S, selected, message, sizeof(message)) && *message && preparedCount(full) == 7);
        std::printf("  refused: %s\n", message);
        CHECK(f->discard(full.p, 6));
        CHECK(prepareSnapshot(full, S, selected) && preparedCount(full) == 8);
    }

    // A sound whose configuration another sound already has gets its engine's tone wheels
    // copied from that one's, and sounds as one built from scratch.
    void sharedWheels()
    {
        const oscairn_sound_parameter_t flute[] = {{param("Upper 16'"), 0}, {param("Upper 8'"), 8}, {param("Upper 4'"), 8}};
        const oscairn_sound_parameter_t square[] = {{param("Upper 8'"), 8}, {param("Wheel shape"), 2}};
        Instance o(48000);
        auto* c = o.ext<oscairn_plugin_sound_contexts_t>(OSCAIRN_EXT_SOUND_CONTEXTS);
        // CPU time of this thread, so other processes on the machine do not change the comparison
        auto cpuMs = [] { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; };
        auto timed = [&](const char* name, const oscairn_sound_parameter_t* v, uint32_t n) {
            const double t = cpuMs();
            const auto id = c->prepare(o.p, name, v, n, nullptr, nullptr, nullptr, 0);
            const double ms = cpuMs() - t;
            std::printf("  %s prepared in %.1f ms of CPU time\n", name, ms);
            CHECK(id != CLAP_INVALID_ID);
            return ms;
        };
        const auto before = organ::enginesShared(o.p);
        timed("flute (the Default's configuration)", flute, 3);
        const auto a = c->prepare(o.p, "flute", flute, 3, nullptr, nullptr, nullptr, 0);
        CHECK(organ::enginesShared(o.p) == before + 2);
        const auto own = timed("square wheels (a configuration of its own)", square, 2);
        CHECK(organ::enginesShared(o.p) == before + 2);
        const auto copied = timed("square wheels again", square, 2);
        CHECK(organ::enginesShared(o.p) == before + 3);
        CHECK(copied < own / 2); // the waves are most of the work: about 2.5 against 42 ms (48 kHz, M-series)
        // The flute from copied wheels against the flute on the Default's own engine.
        Instance ref(48000);
        for(const auto& v : flute) ref.at(0).param(v.id, v.value);
        ref.render(512, 512);
        CHECK(c->select(o.p, a, -1));
        o.render(512, 512);
        for(auto* i : {&o, &ref}) i->at(0).note(CLAP_EVENT_NOTE_ON, 0, 60, 1).note(CLAP_EVENT_NOTE_ON, 0, 67, 2);
        const auto x = o.render(24000, 480).left, y = ref.render(24000, 480).left;
        CHECK(x == y && rms(x, 12000, 24000) > 1e-3);
    }

    // Overdrive at its defaults is about as loud as clean, as in setBfree (its preamp's own
    // starting gains, not the documented 0.3567 and 0.07873).
    void overdrive()
    {
        double level[2];
        for(int on : {0, 1})
        {
            Instance o(48000);
            o.at(0).param(param("Overdrive"), on);
            for(int16_t k : {48, 55, 60, 63, 67}) o.at(0).note(CLAP_EVENT_NOTE_ON, 0, k, k);
            const auto x = o.render(48000, 512).left;
            level[on] = rms(x, 24000, 48000);
        }
        std::printf("  clean %.4f, overdrive %.4f\n", level[0], level[1]);
        CHECK(level[1] > 0.4 * level[0] && level[1] < 1.5 * level[0]);
    }

    const std::map<std::string, std::function<void()>> tests = {
        {"overdrive", overdrive},
        {"presets", presets},
        {"shared-wheels", sharedWheels},
        {"snapshots", snapshots},
        {"memory", memory},
        {"startup", startup},
        {"rates", rates},
        {"click", clickRates},
        {"voices", voices},
        {"transpose", transpose},
        {"quiet", quiet},
        {"threads", threads},
        {"metadata", metadata}, {"determinism", determinism}, {"realtime", realtime}, {"state", state},
        {"notes", notes}, {"choke", chokeAndVolume}, {"contexts", soundContexts}, {"sleep", sleep},
        {"performance", performanceControls}, {"rebuild", rebuildWhilePlaying},
    };
}

extern const std::map<std::string, void (*)()> leslieTests; // leslie_test.cpp

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    bool ran = false;
    std::map<std::string, std::function<void()>> all(tests.begin(), tests.end());
    for(const auto& [name, test] : leslieTests) all[name] = test;
    for(const auto& [name, test] : all)
    {
        if(which != "all" && which != name) continue;
        ran = true;
        const int before = failures;
        std::printf("%s\n", name.c_str());
        test();
        std::printf("%s: %s\n", name.c_str(), failures == before ? "ok" : "FAILED");
    }
    if(!ran) { std::printf("no test %s\n", which.c_str()); return 2; }
    return failures ? 1 : 0;
}
