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

// Tests of setBfree Leslie, the rotary speaker as an effect, through its CLAP interface.

#include "harness.h"

#include "engine.h"
#include "leslie.h"
#include "params.h"

#include <clap/ext/tail.h>
#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

extern "C" const clap_plugin_entry_t clap_entry;

using namespace organtest;

namespace
{
    struct Effect : Instance
    {
        explicit Effect(double rate, bool activate = true) : Instance(rate, activate, leslie::create) {}
    };

    uint32_t param(const char* name)
    {
        auto* p = leslie::create(&host.host);
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

    // A sine on both channels from `from` seconds until `to`.
    auto sine(double hz, double rate, double level = 0.5, double from = 0, double to = 1e9)
    {
        return [=](uint64_t i, float& l, float& r) {
            const double t = double(i) / rate;
            l = r = t >= from && t < to ? float(level * std::sin(2 * M_PI * hz * t)) : 0.f;
        };
    }

    bool reported(const std::vector<std::vector<uint8_t>>& events, clap_id id, double value)
    {
        for(const auto& e : events)
        {
            auto* h = reinterpret_cast<const clap_event_header_t*>(e.data());
            if(h->type != CLAP_EVENT_PARAM_VALUE) continue;
            auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
            if(v->param_id == id && v->value == value) return true;
        }
        return false;
    }

    // The bundle lists both plugins; the effect's parameters are the organ's Leslie parameters,
    // then its own Mix.
    void metadata()
    {
        auto* factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        CHECK(factory && factory->get_plugin_count(factory) == 2);
        const auto* d = factory->get_plugin_descriptor(factory, 1);
        CHECK(d && !std::strcmp(d->id, "org.oscairn.setbfree.leslie") && !std::strcmp(d->name, "setBfree Leslie"));
        CHECK(!std::strcmp(factory->get_plugin_descriptor(factory, 0)->id, "org.oscairn.setbfree"));
        bool effect = false;
        for(auto* f = d->features; *f; ++f) effect |= !std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
        CHECK(effect);
        for(const char* id : {"org.oscairn.setbfree", "org.oscairn.setbfree.leslie"})
        {
            auto* p = factory->create_plugin(factory, &host.host, id);
            CHECK(p && !std::strcmp(p->desc->id, id) && p->init(p));
            if(p) p->destroy(p);
        }

        Effect o(48000);
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        auto* ui = o.ext<oscairn_plugin_ui_t>(OSCAIRN_EXT_UI);
        CHECK(params && ui && o.ext<clap_plugin_tail_t>(CLAP_EXT_TAIL) && o.ext<oscairn_plugin_memory_t>(OSCAIRN_EXT_MEMORY));
        uint32_t organLeslie = 0;
        for(const auto& p : organ::params) organLeslie += !std::strncmp(p.module, "Leslie", 6);
        CHECK(params->count(o.p) == organLeslie); // all but the organ's Leslie switch, and Mix
        for(uint32_t i = 0; i < params->count(o.p); ++i)
        {
            clap_param_info_t info;
            CHECK(params->get_info(o.p, i, &info) && info.id == i + 1);
            oscairn_param_hint_t hint;
            CHECK(ui->param_hint(o.p, info.id, &hint) && hint.priority == info.id);
            if(i + 1 == params->count(o.p))
            {
                CHECK(!std::strcmp(info.name, "Mix") && !std::strcmp(info.module, "Leslie") && info.min_value == 0 && info.max_value == 1 &&
                      info.default_value == 1 && !(info.flags & CLAP_PARAM_IS_STEPPED) && hint.role == OSCAIRN_ROLE_MIX);
                char text[256];
                double back;
                CHECK(params->value_to_text(o.p, info.id, 0.25, text, sizeof(text)) && !std::strcmp(text, "25.0%"));
                CHECK(params->text_to_value(o.p, info.id, text, &back) && back == 0.25);
                continue;
            }
            CHECK(hint.role == OSCAIRN_ROLE_NONE);
            const auto& same = organ::params[organ::index(info.name)];
            CHECK(!std::strcmp(info.module, same.module) && info.min_value == same.min && info.max_value == same.max && info.default_value == same.def);
            for(double v : {info.min_value, info.default_value, info.max_value})
            {
                char text[256], again[256];
                double back;
                CHECK(params->value_to_text(o.p, info.id, v, text, sizeof(text)) && params->text_to_value(o.p, info.id, text, &back));
                params->value_to_text(o.p, info.id, back, again, sizeof(again));
                CHECK(!std::strcmp(text, again));
            }
        }
        clap_param_info_t first;
        params->get_info(o.p, 0, &first);
        oscairn_param_hint_t hint;
        ui->param_hint(o.p, 1, &hint);
        CHECK(!std::strcmp(first.name, "Leslie speed") && hint.widget == OSCAIRN_WIDGET_BUTTONS);
        auto* audio = o.ext<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
        clap_audio_port_info_t in, out;
        CHECK(audio->count(o.p, true) == 1 && audio->count(o.p, false) == 1);
        CHECK(audio->get(o.p, 0, true, &in) && audio->get(o.p, 0, false, &out) && in.channel_count == 2 && out.channel_count == 2);
        auto* notes = o.ext<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
        clap_note_port_info_t np;
        CHECK(notes->count(o.p, true) == 1 && notes->get(o.p, 0, true, &np) && (np.supported_dialects & CLAP_NOTE_DIALECT_MIDI));
    }

    // No allocation in process: speed changes, every parameter, rebuilt rotors, MIDI, reset.
    void realtime()
    {
        Effect o(48000);
        o.input = sine(440, 48000);
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        resetAllocations();
        o.render(4800, 256);
        for(double which : {1.0, 0.0})
        {
            for(uint32_t i = 0; i < params->count(o.p); ++i)
            {
                clap_param_info_t info;
                params->get_info(o.p, i, &info);
                o.at(i).param(info.id, which ? info.max_value : info.min_value);
            }
            o.render(4800, 256);
            o.service(); // builds the new rotor
            o.render(4800, 333);
            o.service(); // frees the old one
        }
        o.at(10).midi(0xB0, 1, 127).at(500).midi(0xB0, 64, 127).at(900).midi(0xB0, 64, 0).midi(0xB0, 1, 0);
        o.render(4800, 128);
        o.p->reset(o.p);
        o.render(4800, 64);
        std::printf("  allocations in process: %d, frees: %d\n", allocations(), frees());
        if(!allocationGuard) { std::printf("  (allocation guard off in this build)\n"); return; }
        CHECK(allocations() == 0 && frees() == 0);
    }

    // State: text, the same bytes back; a loaded state sounds the same; bad states change nothing.
    void state()
    {
        Effect a(48000);
        a.at(0).param(param("Leslie speed"), 2).param(param("Horn fast speed"), 400.125).param(param("Drum filter type"), 5);
        a.render(256, 256);
        const auto saved = a.save();
        CHECK(saved.rfind("setbfree-leslie-2\n", 0) == 0);
        std::vector<float> renders[2];
        for(auto& r : renders)
        {
            Effect b(48000, false);
            CHECK(b.load(saved));
            CHECK(b.save() == saved);
            b.p->activate(b.p, 48000, 1, 4096);
            b.input = sine(1000, 48000);
            r = b.render(48000, 512).left;
        }
        CHECK(renders[0] == renders[1] && rms(renders[0], 24000, 48000) > 0.05);
        Effect c(48000);
        const auto before = c.save();
        std::string bad = saved;
        bad.replace(bad.find("400.125"), 7, "9999");
        CHECK(!c.load(bad) && !c.load(saved.substr(0, saved.size() / 2)) && !c.load("setbfree-leslie-1\n"));
        CHECK(c.save() == before);
        CHECK(c.load(saved));
        double v;
        CHECK(c.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS)->get_value(c.p, param("Horn fast speed"), &v) && v == 400.125);
    }

    // The horn's speed, the cabinet's level and its response are the same at every rate, the
    // horn's highs as at 44.1 kHz (whirl.reference.rate).
    void rates()
    {
        struct M { double rpm, level, high; };
        auto measure = [](double rate) {
            M m{};
            {
                Effect o(rate);
                o.input = sine(1760, rate);
                o.at(0).param(param("Leslie speed"), 2);
                m.rpm = rotorRate(o.render(uint32_t(rate * 6), 512).left, size_t(rate * 3), rate) * 60;
            }
            for(double hz : {300.0, 3000.0})
            {
                Effect o(rate);
                o.input = sine(hz, rate);
                o.at(0).param(param("Leslie speed"), 0);
                const auto x = o.render(uint32_t(rate), 512).left;
                (hz < 1000 ? m.level : m.high) = rms(x, size_t(rate / 2), x.size());
            }
            return m;
        };
        const auto ref = measure(48000), at44 = measure(44100);
        for(double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        {
            const auto m = measure(rate);
            const double high = 20 * std::log10(m.high / at44.high);
            std::printf("  %.0f Hz: horn %.1f RPM, stopped: 300 Hz at %.4f, 3 kHz at %.4f (%+.2f dB)\n", rate, m.rpm, m.level, m.high, high);
            CHECK(std::fabs(m.rpm - ref.rpm) < 0.02 * ref.rpm && std::fabs(m.rpm - 423.36) < 0.03 * 423.36);
            CHECK(std::fabs(m.level / ref.level - 1) < 0.05);
            CHECK(std::fabs(high) < 0.75);
        }
    }

    // The mod wheel and the sustain pedal switch the speed, and say so; Stop stops the horn.
    void speed()
    {
        Effect o(48000);
        o.input = sine(1760, 48000);
        auto* params = o.ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        const auto id = param("Leslie speed");
        double v;
        o.at(0).midi(0xB0, 1, 100);
        auto r = o.render(512, 512);
        CHECK(params->get_value(o.p, id, &v) && v == 2 && reported(r.events, id, 2));
        o.at(0).midi(0xB0, 1, 90); // no crossing: nothing changes
        r = o.render(512, 512);
        CHECK(!reported(r.events, id, 2));
        o.at(0).midi(0xB0, 1, 10);
        r = o.render(512, 512);
        CHECK(params->get_value(o.p, id, &v) && v == 1 && reported(r.events, id, 1));
        o.at(0).midi(0xB0, 64, 127);
        r = o.render(512, 512);
        CHECK(params->get_value(o.p, id, &v) && v == 2);
        o.at(0).midi(0xB0, 64, 0).midi(0xB0, 64, 127);
        r = o.render(512, 512);
        CHECK(params->get_value(o.p, id, &v) && v == 1);
        // Fast, then stopped: the pitch stops moving.
        o.at(0).param(id, 2);
        auto x = o.render(48000 * 4, 512).left;
        const double fast = rotorRate(x, 48000 * 2, 48000) * 60;
        o.at(0).param(id, 0);
        x = o.render(48000 * 6, 512).left;
        double lo = 1e9, hi = 0;
        for(size_t i = 48000 * 4; i + 480 <= x.size(); i += 240)
        {
            const double f = frequency(x, i, i + 480, 48000);
            lo = std::min(lo, f); hi = std::max(hi, f);
        }
        std::printf("  fast %.1f RPM; stopped, the pitch moves %.3f Hz\n", fast, hi - lo);
        CHECK(fast > 400 && hi - lo < 0.5);
    }

    // A parameter that builds a new rotor crossfades to it, the rotors still turning.
    void rebuild()
    {
        Effect o(48000);
        o.input = sine(1760, 48000);
        o.at(0).param(param("Leslie speed"), 2);
        const auto before = o.render(48000 * 3, 480).left;
        const int callbacks = host.callbacks;
        o.at(0).param(param("Horn fast speed"), 350);
        o.render(480, 480);
        CHECK(host.callbacks > callbacks);
        o.service();
        auto x = o.render(48000 * 3, 480).left;
        double step = 0, held = 0;
        for(size_t i = 1; i < 4800; ++i) step = std::max(step, std::fabs(double(x[i]) - x[i - 1]));
        for(size_t i = before.size() - 48000; i < before.size(); ++i) held = std::max(held, std::fabs(double(before[i]) - before[i - 1]));
        const double rpm = rotorRate(x, 48000, 48000) * 60;
        std::printf("  largest step across the crossfade %.4f (playing: %.4f); horn %.1f RPM after\n", step, held, rpm);
        CHECK(step < 1.5 * held);
        CHECK(std::fabs(rpm - 350) < 10);
        // The new rotor takes over the old one's angle and speed.
        sbf_rotor* a = sbf_rotor_create(48000, nullptr);
        sbf_rotor* b = sbf_rotor_create(48000, "whirl.horn.fastrpm=350\n");
        sbf_rotor_speed(a, 2);
        sbf_rotor_speed(b, 2);
        float in[SBF_FRAGMENT] = {}, l[SBF_FRAGMENT], r[SBF_FRAGMENT];
        for(int i = 0; i < 1000; ++i) sbf_rotor_process(a, in, l, r, SBF_FRAGMENT);
        sbf_rotor_follow(b, a);
        CHECK(sbf_rotor_horn_rpm(a) > 400 && sbf_rotor_horn_rpm(b) == sbf_rotor_horn_rpm(a));
        sbf_rotor_destroy(a);
        sbf_rotor_destroy(b);
    }

    // The tail: after the input stops the output dies away within the reported tail, and then
    // the effect sleeps; reset empties it at once.
    // At 192 kHz the largest cabinet no longer overruns the 2048-sample delay lines: nothing
    // wraps round to arrive within 2 ms of a click, before the cabinet's first reflections.
    void cabinet()
    {
        for(double rate : {96000.0, 192000.0})
        {
            Effect o(rate);
            const auto period = uint64_t(rate * 0.02);
            o.input = [=](uint64_t i, float& l, float& r) { l = r = i % period ? 0.f : 1.f; };
            o.at(0).param(param("Mic distance"), 100).param(param("Horn radius"), 50).param(param("Drum radius"), 50)
                .param(param("Horn leakage"), 0).param(param("Leslie speed"), 1);
            o.render(512, 512);
            o.service();
            const auto x = o.render(uint32_t(rate * 2), 512);
            double early = 0, all = 0;
            for(size_t i = size_t(rate * 0.1); i < x.left.size(); ++i)
            {
                const double e = double(x.left[i]) * x.left[i] + double(x.right[i]) * x.right[i];
                all += e;
                if((512 + i) % period < uint64_t(rate * 0.002)) early += e;
            }
            std::printf("  %.0f Hz, largest cabinet: %.2g of the energy within 2 ms of a click\n", rate, early / all);
            CHECK(all > 0 && early < 1e-4 * all);
        }
    }

    // Mix: 0 is the input as it came, both channels; between, the dry and wet in proportion; a
    // change moves over 10 ms (from the default, 1, at the start too).
    void mix()
    {
        const double rate = 48000;
        auto input = [](uint64_t i, float& l, float& r) { l = float(0.5 * std::sin(i * 0.05)); r = float(0.3 * std::sin(i * 0.031)); };
        auto play = [&](double mix, double later = -1) {
            Effect o(rate);
            o.input = input;
            o.at(0).param(param("Leslie speed"), 2).param(param("Mix"), mix);
            auto x = o.render(4800, 512);
            if(later >= 0) o.at(0).param(param("Mix"), later);
            const auto y = o.render(4800, 512);
            x.left.insert(x.left.end(), y.left.begin(), y.left.end());
            x.right.insert(x.right.end(), y.right.begin(), y.right.end());
            return x;
        };
        const auto dry = play(0), wet = play(1), half = play(0.5), change = play(1, 0);
        double most = 0, step = 0;
        for(size_t i = 480; i < dry.left.size(); ++i)
        {
            float l, r;
            input(i, l, r);
            CHECK(dry.left[i] == l && dry.right[i] == r);
            most = std::max({most, std::fabs(half.left[i] - 0.5 * (l + wet.left[i])), std::fabs(half.right[i] - 0.5 * (r + wet.right[i]))});
            if(i < 4800) CHECK(change.left[i] == wet.left[i]);
            if(i >= 4800 + 480) CHECK(change.left[i] == l && change.right[i] == r);
        }
        const double at = (change.left[4800 + 240] - dry.left[4800 + 240]) / (wet.left[4800 + 240] - dry.left[4800 + 240]);
        std::printf("  half: within %.2g of the mean of dry and wet; 5 ms into a change from 1 to 0: %.3f wet\n", most, at);
        CHECK(most < 1e-6 && std::fabs(at - 0.5) < 0.01 && rms(wet.left, 0, wet.left.size()) > 0.05);
    }

    void tail()
    {
        for(double rate : {44100.0, 192000.0})
        {
            Effect o(rate);
            o.input = [rate](uint64_t i, float& l, float& r) { l = r = i < uint64_t(rate / 5) ? float(0.5 * std::sin(i * 0.37)) * (i % 7 ? 1.f : -1.f) : 0.f; };
            const uint32_t tailFrames = o.ext<clap_plugin_tail_t>(CLAP_EXT_TAIL)->get(o.p);
            CHECK(tailFrames == uint32_t(std::ceil(rate * 0.05)));
            const uint32_t block = 64;
            Output out;
            uint64_t sleptAt = 0;
            for(uint64_t at = 0; at < uint64_t(rate) && !sleptAt; at += block)
                if(o.process(block, &out) == CLAP_PROCESS_SLEEP) sleptAt = at + block;
            size_t last = 0;
            for(size_t i = 0; i < out.left.size(); ++i) if(std::fabs(out.left[i]) >= 1e-5f || std::fabs(out.right[i]) >= 1e-5f) last = i;
            const uint64_t stop = uint64_t(rate / 5);
            std::printf("  %.0f Hz: the output dies away %.1f ms after the input; asleep %.1f ms after (tail %u frames)\n",
                        rate, (double(last) - stop) * 1000 / rate, (double(sleptAt) - stop) * 1000 / rate, tailFrames);
            CHECK(sleptAt > stop && last < stop + tailFrames && sleptAt <= stop + tailFrames + 2 * block);
            oscairn_effect_resources_t res;
            CHECK(o.ext<oscairn_plugin_effect_contexts_t>(OSCAIRN_EXT_EFFECT_CONTEXTS)->resources(o.p, &res) && !res.tail_active);
        }
        Effect o(48000);
        o.input = sine(440, 48000, 0.5, 0, 0.1);
        o.render(4800, 480);
        o.p->reset(o.p);
        const auto x = o.render(480, 480).left;
        CHECK(rms(x, 0, x.size()) < 1e-9); // whirl.c adds a denormal guard to its input
    }

    // Effect contexts: up to 16 rotors on one engine, each with its own parameters and state,
    // at the engine's rate; they outlive the interface they came from.
    void contexts()
    {
        auto* first = new Effect(48000);
        auto* ext = first->ext<oscairn_plugin_effect_contexts_t>(OSCAIRN_EXT_EFFECT_CONTEXTS);
        CHECK(ext && ext->create && ext->resources);
        std::vector<const clap_plugin_t*> made;
        while(const auto* c = ext->create(first->p, &host.host)) { made.push_back(c); if(made.size() > 20) break; }
        CHECK(made.size() == 15);
        oscairn_effect_resources_t res;
        CHECK(ext->resources(first->p, &res) && res.capacity == 16 && res.prepared == 16 && res.shared_bytes > 0);
        for(auto* c : made) CHECK(c->init(c));
        CHECK(!made[0]->activate(made[0], 96000, 1, 4096)); // one rate per engine
        for(auto* c : made) CHECK(c->activate(c, 48000, 1, 4096) && c->start_processing(c));
        CHECK(ext->resources(made[0], &res) && res.context_bytes > 1000000);
        auto* params = first->ext<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        const clap_event_param_value_t fast{{sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, 1, nullptr, -1, -1, -1, -1, 2};
        const clap_input_events_t in{const_cast<clap_event_param_value_t*>(&fast), [](const clap_input_events_t*) -> uint32_t { return 1; },
                                     [](const clap_input_events_t* l, uint32_t) { return static_cast<const clap_event_header_t*>(l->ctx); }};
        params->flush(made[3], &in, nullptr);
        double v;
        CHECK(params->get_value(made[3], 1, &v) && v == 2 && params->get_value(made[4], 1, &v) && v == 1);
        delete first; // the others keep the engine
        std::vector<float> il(512), ir(512), l(512), r(512);
        for(size_t i = 0; i < il.size(); ++i) il[i] = ir[i] = float(0.5 * std::sin(i * 0.2));
        float* ins[2] = {il.data(), ir.data()};
        float* outs[2] = {l.data(), r.data()};
        clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
        const clap_input_events_t none{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; }, nullptr};
        clap_process_t proc{};
        proc.steady_time = -1; proc.frames_count = 512; proc.audio_inputs = &ib; proc.audio_inputs_count = 1;
        proc.audio_outputs = &ob; proc.audio_outputs_count = 1; proc.in_events = &none;
        for(auto* c : made) CHECK(c->process(c, &proc) == CLAP_PROCESS_CONTINUE);
        CHECK(ext->resources(made[0], &res) && res.prepared == 15 && res.tail_active);
        for(auto* c : made) { c->stop_processing(c); c->deactivate(c); c->destroy(c); }
    }

    // Memory: the estimate covers what activating allocates, within 25 %.
    void memory()
    {
        if(!allocationGuard) { std::printf("  (needs the allocation guard's allocator)\n"); return; }
        for(double rate : {44100.0, 192000.0})
        {
            Effect o(rate, false);
            auto* mem = o.ext<oscairn_plugin_memory_t>(OSCAIRN_EXT_MEMORY);
            oscairn_memory_t e{};
            CHECK(mem->estimate(o.p, rate, 4096, &e));
            trackBytes(true);
            o.p->activate(o.p, rate, 1, 4096);
            trackBytes(false);
            const double estimate = double(e.shared_bytes + e.context_bytes), held = double(trackedBytes());
            std::printf("  %.0f Hz: estimate %.3f MB, allocated %.3f MB\n", rate, estimate / 1e6, held / 1e6);
            CHECK(estimate >= held && estimate <= 1.25 * held);
            oscairn_memory_t now{};
            CHECK(mem->current(o.p, &now) && now.context_bytes == e.context_bytes);
            o.p->start_processing(o.p);
        }
    }

    // The same input renders the same in fresh instances and in any block size.
    void determinism()
    {
        std::vector<float> x[3];
        const uint32_t blocks[] = {512, 512, 37};
        for(int i = 0; i < 3; ++i)
        {
            Effect o(48000);
            o.input = sine(700, 48000);
            o.at(4000).param(param("Leslie speed"), 2).at(40000).midi(0xB0, 64, 127);
            x[i] = o.render(96000, blocks[i]).left;
        }
        double d = 0;
        for(size_t i = 0; i < x[0].size(); ++i) d += double(x[0][i] - x[2][i]) * (x[0][i] - x[2][i]);
        d = std::sqrt(d / double(x[0].size()));
        std::printf("  37- vs 512-frame blocks: rms difference %.2g (signal %.3f)\n", d, rms(x[0], 0, x[0].size()));
        CHECK(x[0] == x[1] && x[0] == x[2]);
    }

    // The main thread services, makes and frees contexts and reads their resources while the
    // audio thread plays and changes parameters (run under ThreadSanitizer in CI).
    void threads()
    {
        Effect o(48000);
        o.input = sine(440, 48000);
        auto* ext = o.ext<oscairn_plugin_effect_contexts_t>(OSCAIRN_EXT_EFFECT_CONTEXTS);
        auto* mem = o.ext<oscairn_plugin_memory_t>(OSCAIRN_EXT_MEMORY);
        const auto horn = param("Horn slow speed"), speed = param("Leslie speed");
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> blocks{0};
        std::thread audio([&] {
            for(uint32_t i = 0; !stop; ++i)
            {
                if(i % 5 == 0) o.at(3).param(horn, 20 + i % 50);
                if(i % 7 == 0) o.at(9).param(speed, i % 3);
                o.process(256);
                ++blocks;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
        for(int round = 0; round < 200; ++round)
        {
            o.service();
            oscairn_effect_resources_t res;
            oscairn_memory_t m;
            ext->resources(o.p, &res);
            mem->current(o.p, &m);
            if(round % 20 == 0)
                if(auto* c = ext->create(o.p, &host.host)) { c->init(c); c->activate(c, 48000, 1, 512); c->deactivate(c); c->destroy(c); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop = true;
        audio.join();
        o.service();
        std::printf("  %u blocks\n", blocks.load());
        CHECK(blocks > 100);
    }
}

extern const std::map<std::string, void (*)()> leslieTests;
const std::map<std::string, void (*)()> leslieTests = {
    {"leslie-metadata", metadata}, {"leslie-realtime", realtime}, {"leslie-state", state}, {"leslie-rates", rates},
    {"leslie-speed", speed}, {"leslie-rebuild", rebuild}, {"leslie-tail", tail}, {"leslie-contexts", contexts},
    {"leslie-memory", memory}, {"leslie-determinism", determinism}, {"leslie-threads", threads}, {"leslie-cabinet", cabinet},
    {"leslie-mix", mix},
};
