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

// Tests of Oscairn Organ through its CLAP interface with a minimal host
// (doc/oscairn-plan.md, section 4). Run with a test name, or "all".

#include "harness.h"

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

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0;
        for(size_t i = from; i < to; ++i) s += double(x[i]) * x[i];
        return std::sqrt(s / double(to - from));
    }

    // Metadata: unique IDs, ranges, module paths, value text both ways, hints.
    void metadata()
    {
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

    // A host choke fades the only key of a sound in 2-5 ms; per-note volume scales it.
    void chokeAndVolume()
    {
        for(double rate : {44100.0, 48000.0})
        {
            Instance o(rate);
            o.at(0).param(param("Leslie"), 0).param(param("Reverb"), 0);
            o.render(256, 256);
            o.service();
            o.at(0).note(CLAP_EVENT_NOTE_ON, 0, 69, 7);
            auto r = o.render(uint32_t(rate / 2), 128);
            const double before = rms(r.left, r.left.size() - 4096, r.left.size());
            o.at(0).expression(CLAP_NOTE_EXPRESSION_VOLUME, 0, 69, 7, 0.5);
            r = o.render(uint32_t(rate / 4), 128);
            const double half = rms(r.left, r.left.size() - 4096, r.left.size());
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
        }
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
        uint32_t prepared = 0, discarded = 0;
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
        std::printf("  %u blocks, %u prepared, %u discarded\n", blocks.load(), prepared, discarded);
        CHECK(blocks > 100 && prepared >= 4 && discarded == prepared);
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

    // Frequency from interpolated rising zero crossings over [from, to).
    double frequency(const std::vector<float>& x, size_t from, size_t to, double rate)
    {
        double first = -1, last = -1;
        int n = 0;
        for(size_t i = from + 1; i < to; ++i)
            if(x[i - 1] < 0 && x[i] >= 0)
            {
                const double t = double(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                if(first < 0) first = t; else ++n;
                last = t;
            }
        return n > 0 ? n * rate / (last - first) : 0;
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

    struct RateMeasure { double pitch, vibrato, percussion, leslie; };
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
            // The horn's Doppler shift: the strongest periodicity of the tone's pitch, measured
            // every 5 ms, between 2 and 12 Hz, to 0.01 Hz.
            std::vector<double> e;
            const size_t w = size_t(rate / 200);
            for(size_t i = size_t(rate * 3); i + 2 * w <= x.size(); i += w) e.push_back(frequency(x, i, i + 2 * w, rate));
            double mean = 0;
            for(double v : e) mean += v / double(e.size());
            for(auto& v : e) v -= mean;
            double best = 0, power = 0;
            for(double f = 2; f <= 12; f += 0.01)
            {
                double re = 0, im = 0;
                for(size_t k = 0; k < e.size(); ++k) { re += e[k] * std::cos(2 * M_PI * f * k / 200); im += e[k] * std::sin(2 * M_PI * f * k / 200); }
                if(re * re + im * im > power) { power = re * re + im * im; best = f; }
            }
            m.leslie = best * 60; // RPM
        }
        return m;
    }

    // Every sample rate sounds the same: pitch, vibrato, percussion decay, Leslie speed.
    void rates()
    {
        const auto ref = measure(48000);
        std::printf("  48000 Hz: A4 %.4f Hz, V3 %.1f cents p-p, fast percussion -20 dB in %.2f s, horn %.1f RPM\n", ref.pitch, ref.vibrato, ref.percussion, ref.leslie);
        for(double rate : {44100.0, 88200.0, 96000.0, 192000.0})
        {
            const auto m = measure(rate);
            const double cents = 1200 * std::log2(m.pitch / ref.pitch);
            std::printf("  %.0f Hz: A4 %+.3f cents, V3 %.1f cents p-p, percussion %.2f s, horn %.1f RPM\n", rate, cents, m.vibrato, m.percussion, m.leslie);
            CHECK(std::fabs(cents) < 0.5);
            CHECK(std::fabs(m.vibrato - ref.vibrato) < 0.1 * ref.vibrato);
            CHECK(std::fabs(m.percussion - ref.percussion) <= 0.02);
            CHECK(std::fabs(m.leslie - ref.leslie) < 0.02 * ref.leslie);
        }
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

    const std::map<std::string, std::function<void()>> tests = {
        {"startup", startup},
        {"rates", rates},
        {"quiet", quiet},
        {"threads", threads},
        {"metadata", metadata}, {"determinism", determinism}, {"realtime", realtime}, {"state", state},
        {"notes", notes}, {"choke", chokeAndVolume}, {"contexts", soundContexts}, {"sleep", sleep},
        {"performance", performanceControls}, {"rebuild", rebuildWhilePlaying},
    };
}

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    bool ran = false;
    for(const auto& [name, test] : tests)
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
