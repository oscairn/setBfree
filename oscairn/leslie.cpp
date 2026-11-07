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

// setBfree Leslie: setBfree's rotary speaker (b_whirl) as a CLAP audio effect, with the
// organ's Leslie parameters under the same names (doc/oscairn-plan.md, section 6).
//
// The input's two channels are summed into the cabinet; the horn and drum come out on two
// microphones. Leslie speed is live; the other parameters build a new rotor on the main
// thread, which takes over the old one's rotor angles and speeds and is crossfaded in over
// 10 ms. Each effect context (oscairn.effect-contexts) owns one rotor. The mod wheel and the
// sustain pedal switch the speed, as on the organ. Mix, the effect's own parameter (last),
// blends the dry input back in.

#include "leslie.h"
#include "engine.h"
#include "params.h"

#include <clap/ext/tail.h>
#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <locale>
#include <memory>
#include <sstream>
#include <string>

namespace leslie
{
namespace
{
    using organ::Param;

    constexpr bool same(const char* a, const char* b)
    {
        while(*a && *a == *b) { ++a; ++b; }
        return *a == *b;
    }
    // The organ's Leslie parameters, but its on/off switch (an effect is bypassed by its host).
    constexpr bool isLeslie(const Param& p)
    {
        const char* m = p.module;
        return m[0] == 'L' && m[1] == 'e' && m[2] == 's' && m[3] == 'l' && m[4] == 'i' && m[5] == 'e' && !same(p.name, "Leslie");
    }
    constexpr uint32_t organCount = [] {
        uint32_t n = 0;
        for(const auto& p : organ::params) n += isLeslie(p);
        return n;
    }();
    constexpr std::array<uint32_t, organCount> organIndex = [] {
        std::array<uint32_t, organCount> a{};
        uint32_t n = 0;
        for(uint32_t i = 0; i < organ::paramCount; ++i) if(isLeslie(organ::params[i])) a[n++] = i;
        return a;
    }();
    // Dry/wet, after the organ's parameters so that their IDs stay the same.
    constexpr Param mix{"Mix", "Leslie", 0, 1, 1, false, organ::Kind::Plugin, organ::Map::Unit, nullptr, organ::Text::Percent, "",
                        nullptr, nullptr, organ::W_KNOB, 0, 0, OSCAIRN_ROLE_MIX};
    constexpr uint32_t count = organCount + 1, iMix = organCount;
    const Param& param(uint32_t i) { return i == iMix ? mix : organ::params[organIndex[i]]; }
    constexpr uint32_t iSpeed = [] {
        for(uint32_t i = 0; i < organCount; ++i) if(same(organ::params[organIndex[i]].name, "Leslie speed")) return i;
        return organCount;
    }();
    static_assert(iSpeed == 0, "Leslie speed comes first");

    constexpr uint32_t F = SBF_FRAGMENT, capacity = 16;
    constexpr float quietPeak = 1e-5f; // -100 dBFS
    constexpr double tailSeconds = 0.05; // the delay lines and filters ring out well within this
    // What a rotor allocates (measured on macOS with malloc_size); the tests keep it at or above.
    constexpr uint64_t rotorBytes = 1100000;

    const char* const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_TREMOLO, CLAP_PLUGIN_FEATURE_CHORUS, "rotary speaker", CLAP_PLUGIN_FEATURE_STEREO, nullptr};

    // What the contexts of one engine share: the rate.
    struct Engine
    {
        double rate;
        std::atomic<uint32_t> contexts{0};
        explicit Engine(double r) : rate(r) {}
    };

    // Rotors are made and freed on the main thread; how many are alive, for any thread to read.
    struct Rotors
    {
        std::atomic<uint32_t> alive{0};
        sbf_rotor* make(double rate, const std::string& config)
        {
            auto* r = sbf_rotor_create(rate, config.c_str());
            if(r) alive.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
        void free(sbf_rotor* r)
        {
            if(!r) return;
            sbf_rotor_destroy(r);
            alive.fetch_sub(1, std::memory_order_relaxed);
        }
    };

    // setBfree configuration text for the rotor (C locale).
    std::string configFor(const std::array<double, count>& v)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(17);
        for(uint32_t i = 0; i < count; ++i)
        {
            const auto& p = param(i);
            if(p.kind != organ::Kind::Config) continue;
            out << p.key << '=';
            if(p.map == organ::Map::List) out << std::lround(v[i]);
            else out << v[i];
            out << '\n';
        }
        return out.str();
    }

    struct Leslie
    {
        clap_plugin_t plugin{};
        const clap_host_t* host = nullptr;
        std::shared_ptr<Engine> engine;
        std::array<std::atomic<double>, count> values{};
        std::atomic<bool> active{false};

        // Audio thread while active; main thread otherwise.
        sbf_rotor* rotor = nullptr;
        sbf_rotor* fading = nullptr;
        uint32_t fadeLeft = 0, fadeFrames = 480, tailFrames = 2400, quietFor = 0;
        int speed = -1; // applied to the rotor
        float wet = 1; // the mix, moving to Mix over fadeFrames
        bool loud = false; // the last samples rendered
        bool fresh = false; // nothing rendered since activate or reset: a rotor built now takes over whole
        uint8_t wheel = 0, pedal = 0;
        float in[F], l[F], r[F], fl[F], fr[F];

        std::atomic<sbf_rotor*> pending{nullptr}, retired{nullptr}; // main → audio → main
        std::atomic<uint32_t> wanted{0};
        uint32_t built = 0; // main
        std::atomic<bool> tail{false};
        Rotors rotors;

        explicit Leslie(std::shared_ptr<Engine> e) : engine(std::move(e))
        {
            for(uint32_t i = 0; i < count; ++i) values[i] = param(i).def;
            if(engine) ++engine->contexts;
        }
        ~Leslie()
        {
            rotors.free(rotor);
            rotors.free(fading);
            rotors.free(pending.exchange(nullptr));
            rotors.free(retired.exchange(nullptr));
            if(engine) --engine->contexts;
        }

        std::array<double, count> snapshot() const
        {
            std::array<double, count> v;
            for(uint32_t i = 0; i < count; ++i) v[i] = values[i].load(std::memory_order_relaxed);
            return v;
        }
        // Main thread.
        sbf_rotor* build()
        {
            auto* r = rotors.make(engine->rate, configFor(snapshot()));
            if(r) sbf_rotor_speed(r, int(std::lround(values[iSpeed].load(std::memory_order_relaxed))));
            return r;
        }
        void service()
        {
            rotors.free(retired.exchange(nullptr, std::memory_order_acq_rel));
            if(!active.load(std::memory_order_acquire)) return;
            const auto want = wanted.load(std::memory_order_acquire);
            if(want == built) return;
            built = want;
            if(auto* r = build()) rotors.free(pending.exchange(r, std::memory_order_acq_rel));
        }
        void askForService()
        {
            if(host && host->request_callback) host->request_callback(host);
        }

        void set(uint32_t i, double v, const clap_output_events_t* out = nullptr, uint32_t time = 0)
        {
            const auto& p = param(i);
            v = std::clamp(v, p.min, p.max);
            if(p.stepped) v = std::round(v);
            if(values[i].load(std::memory_order_relaxed) == v) return;
            values[i].store(v, std::memory_order_relaxed);
            if(p.kind == organ::Kind::Config) { wanted.fetch_add(1, std::memory_order_release); askForService(); }
            if(!out) return;
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = i + 1; ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.value = v;
            out->try_push(out, &ev.header);
        }
        // A parameter event, or the mod wheel or sustain pedal switching the speed (reported).
        void apply(const clap_event_header_t* h, const clap_output_events_t* out)
        {
            if(h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
            if(h->type == CLAP_EVENT_PARAM_VALUE)
            {
                const auto& e = *reinterpret_cast<const clap_event_param_value_t*>(h);
                if(e.note_id >= 0 || e.key >= 0 || e.channel >= 0 || e.port_index > 0) return;
                if(e.param_id >= 1 && e.param_id <= count && std::isfinite(e.value)) set(e.param_id - 1, e.value);
                return;
            }
            if(h->type != CLAP_EVENT_MIDI) return;
            const auto& m = *reinterpret_cast<const clap_event_midi_t*>(h);
            if(m.port_index || (m.data[0] & 0xf0) != 0xb0) return;
            const uint8_t v = m.data[2] & 127;
            if(m.data[1] == 1) // like a half-moon switch: fast from 64 up, slow below, on crossing
            {
                if((wheel < 64) != (v < 64)) set(iSpeed, v >= 64 ? 2 : 1, out, h->time);
                wheel = v;
            }
            else if(m.data[1] == 64) // each press toggles slow and fast
            {
                if(pedal < 64 && v >= 64) set(iSpeed, values[iSpeed].load(std::memory_order_relaxed) == 2 ? 1 : 2, out, h->time);
                pedal = v;
            }
        }

        // Audio thread: a rotor the main thread built takes over, crossfaded from the old one's angles
        // and speeds; just after a reset, at once and as built, as on a fresh instance.
        void adopt()
        {
            if(fading && !fadeLeft && !retired.load(std::memory_order_acquire))
            {
                retired.store(fading, std::memory_order_release);
                fading = nullptr;
                askForService();
            }
            if(fading || (!fresh && retired.load(std::memory_order_acquire)) || !pending.load(std::memory_order_relaxed)) return;
            auto* next = pending.exchange(nullptr, std::memory_order_acq_rel);
            if(!next) return;
            if(!fresh) { sbf_rotor_follow(next, rotor); fadeLeft = fadeFrames; }
            fading = rotor;
            rotor = next;
            speed = -1;
        }
        void render(const float* inL, const float* inR, float* outL, float* outR, uint32_t n)
        {
            const int want = int(std::lround(values[iSpeed].load(std::memory_order_relaxed)));
            if(want != speed) { speed = want; sbf_rotor_speed(rotor, speed); if(fading) sbf_rotor_speed(fading, speed); }
            bool heard = false;
            for(uint32_t k = 0; k < n; ++k)
            {
                in[k] = inL ? (inR ? 0.5f * (inL[k] + inR[k]) : inL[k]) : 0.f;
                heard |= std::fabs(in[k]) > 0.f;
            }
            sbf_rotor_process(rotor, in, l, r, n);
            if(fading && fadeLeft) sbf_rotor_process(fading, in, fl, fr, n);
            const float wetTo = float(values[iMix].load(std::memory_order_relaxed)), step = 1.f / float(fadeFrames);
            float peak = 0;
            for(uint32_t k = 0; k < n; ++k)
            {
                float a = l[k], b = r[k];
                if(fading && fadeLeft)
                {
                    const float g = float(fadeLeft) / float(fadeFrames);
                    a = a * (1 - g) + fl[k] * g;
                    b = b * (1 - g) + fr[k] * g;
                    --fadeLeft;
                }
                wet += std::clamp(wetTo - wet, -step, step);
                const float dryL = inL ? inL[k] : 0.f, dryR = inR ? inR[k] : dryL;
                a = a * wet + dryL * (1 - wet);
                b = b * wet + dryR * (1 - wet);
                outL[k] = a; outR[k] = b;
                peak = std::max({peak, std::fabs(a), std::fabs(b)});
            }
            quietFor = heard ? 0 : std::min(quietFor + n, tailFrames); // the output dies away within the tail
            loud = peak >= quietPeak;
        }
    };

    Leslie& self(const clap_plugin_t* p) { return *static_cast<Leslie*>(p->plugin_data); }

    clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc)
    {
        auto& s = self(p);
        if(!s.rotor || proc->audio_outputs_count < 1 || proc->audio_outputs[0].channel_count < 2) return CLAP_PROCESS_ERROR;
        s.adopt();
        if(s.fresh) s.wet = float(s.values[iMix].load(std::memory_order_relaxed)); // Mix moves only once heard
        const bool hasIn = proc->audio_inputs_count > 0 && proc->audio_inputs[0].channel_count > 0;
        const float* inL = hasIn ? proc->audio_inputs[0].data32[0] : nullptr;
        const float* inR = hasIn && proc->audio_inputs[0].channel_count > 1 ? proc->audio_inputs[0].data32[1] : nullptr;
        float* outL = proc->audio_outputs[0].data32[0];
        float* outR = proc->audio_outputs[0].data32[1];
        const uint32_t frames = proc->frames_count, events = proc->in_events ? proc->in_events->size(proc->in_events) : 0;
        uint32_t e = 0;
        for(uint32_t i = 0; i < frames;)
        {
            for(; e < events; ++e)
            {
                const auto* h = proc->in_events->get(proc->in_events, e);
                if(h->time > i) break;
                s.apply(h, proc->out_events);
            }
            uint32_t to = frames;
            if(e < events) to = std::min(to, std::max(i + 1, proc->in_events->get(proc->in_events, e)->time));
            to = std::min(to, i + F);
            s.render(inL ? inL + i : nullptr, inR ? inR + i : nullptr, outL + i, outR + i, to - i);
            i = to;
        }
        for(; e < events; ++e) s.apply(proc->in_events->get(proc->in_events, e), proc->out_events);
        s.fresh = false;
        const bool ringing = s.quietFor < s.tailFrames || s.loud || s.fading || s.pending.load(std::memory_order_relaxed);
        s.tail.store(ringing, std::memory_order_relaxed);
        return ringing ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
    }

    // Parameters.
    const clap_plugin_params_t paramsExt = {
        [](const clap_plugin_t*) { return count; },
        [](const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
            if(index >= count) return false;
            const auto& p = param(index);
            *info = {};
            info->id = index + 1;
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | (p.stepped ? CLAP_PARAM_IS_STEPPED : 0) | (p.labels ? CLAP_PARAM_IS_ENUM : 0);
            info->min_value = p.min; info->max_value = p.max; info->default_value = p.def;
            std::snprintf(info->name, sizeof(info->name), "%s", p.name);
            std::snprintf(info->module, sizeof(info->module), "%s", p.module);
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, double* value) {
            if(id < 1 || id > count) return false;
            *value = self(p).values[id - 1].load(std::memory_order_relaxed);
            return true;
        },
        [](const clap_plugin_t*, clap_id id, double value, char* text, uint32_t size) {
            return id >= 1 && id <= count && organ::valueToText(param(id - 1), value, text, size);
        },
        [](const clap_plugin_t*, clap_id id, const char* text, double* value) {
            return id >= 1 && id <= count && organ::textToValue(param(id - 1), text, value);
        },
        [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
            for(uint32_t i = 0, n = in ? in->size(in) : 0; i < n; ++i) self(p).apply(in->get(in, i), out);
        },
    };

    // Speed first, then the rotors, cabinet and filters as on the organ, then Mix.
    const oscairn_plugin_ui_t uiExt = {[](const clap_plugin_t*, clap_id id, oscairn_param_hint_t* hint) {
        if(id < 1 || id > count) return false;
        const auto& p = param(id - 1);
        *hint = {p.widget, id, CLAP_INVALID_ID, 0, p.role};
        return true;
    }};

    // State: a version line, then every value with 17 digits in the C locale.
    constexpr char stateVersion[] = "setbfree-leslie-2"; // 2: Mix
    const clap_plugin_state_t stateExt = {
        [](const clap_plugin_t* p, const clap_ostream_t* stream) {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out.precision(17);
            out << stateVersion << '\n';
            for(const auto& v : self(p).values) out << v.load(std::memory_order_relaxed) << '\n';
            const auto data = out.str();
            for(size_t at = 0; at < data.size();)
            {
                const auto n = stream->write(stream, data.data() + at, data.size() - at);
                if(n <= 0 || size_t(n) > data.size() - at) return false;
                at += size_t(n);
            }
            return true;
        },
        [](const clap_plugin_t* p, const clap_istream_t* stream) {
            std::string data;
            char buffer[256];
            for(;;)
            {
                const auto n = stream->read(stream, buffer, sizeof(buffer));
                if(n < 0 || n > int64_t(sizeof(buffer)) || data.size() + size_t(n) > 4096) return false;
                if(!n) break;
                data.append(buffer, size_t(n));
            }
            std::istringstream in(data);
            in.imbue(std::locale::classic());
            std::string version;
            if(!(in >> version) || version != stateVersion) return false;
            std::array<double, count> v;
            for(uint32_t i = 0; i < count; ++i)
            {
                const auto& d = param(i);
                if(!(in >> v[i]) || !std::isfinite(v[i]) || v[i] < d.min || v[i] > d.max || (d.stepped && v[i] != std::round(v[i]))) return false;
            }
            in >> std::ws;
            if(!in.eof()) return false;
            auto& s = self(p);
            for(uint32_t i = 0; i < count; ++i) s.set(i, v[i]);
            s.service(); // a rotor for the state now, ready for the next block
            return true;
        },
    };

    const clap_plugin_audio_ports_t audioPortsExt = {
        [](const clap_plugin_t*, bool) -> uint32_t { return 1; },
        [](const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
            if(index) return false;
            *info = {};
            info->id = 0; info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->channel_count = 2;
            info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
            std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
            return true;
        },
    };
    // MIDI in, for the mod wheel and the sustain pedal.
    const clap_plugin_note_ports_t notePortsExt = {
        [](const clap_plugin_t*, bool isInput) -> uint32_t { return isInput ? 1 : 0; },
        [](const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
            if(!isInput || index) return false;
            *info = {};
            info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_MIDI; info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
            std::snprintf(info->name, sizeof(info->name), "Speed control");
            return true;
        },
    };

    const clap_plugin_tail_t tailExt = {[](const clap_plugin_t* p) { return self(p).tailFrames; }};

    // oscairn.memory: a rotor per context; nothing shared but the engine record.
    const oscairn_plugin_memory_t memoryExt = {
        [](const clap_plugin_t*, double rate, uint32_t, oscairn_memory_t* out) {
            if(!(rate > 0)) return false;
            *out = {sizeof(Engine), sizeof(Leslie) + rotorBytes, 0};
            return true;
        },
        [](const clap_plugin_t* p, oscairn_memory_t* out) {
            auto& s = self(p);
            if(!s.active.load(std::memory_order_acquire)) return false;
            *out = {sizeof(Engine), sizeof(Leslie) + rotorBytes * s.rotors.alive.load(std::memory_order_relaxed), 0};
            return true;
        },
    };

    const clap_plugin_t* make(const clap_host_t* host, std::shared_ptr<Engine> engine);

    const oscairn_plugin_effect_contexts_t contextsExt = {
        [](const clap_plugin_t* p, const clap_host_t* host) -> const clap_plugin_t* {
            auto engine = self(p).engine;
            if(!engine || engine->contexts.load() >= capacity) return nullptr;
            try { return make(host, std::move(engine)); } catch(...) { return nullptr; }
        },
        [](const clap_plugin_t* p, oscairn_effect_resources_t* out) {
            auto& s = self(p);
            if(!out || !s.engine) return false;
            *out = {sizeof(Engine), sizeof(Leslie) + rotorBytes * s.rotors.alive.load(std::memory_order_relaxed), capacity, s.engine->contexts.load(), s.tail.load()};
            return true;
        },
    };

    const clap_plugin_t* make(const clap_host_t* host, std::shared_ptr<Engine> engine)
    {
        auto* s = new Leslie(std::move(engine));
        s->host = host;
        s->plugin.desc = &descriptor;
        s->plugin.plugin_data = s;
        s->plugin.init = [](const clap_plugin_t*) { return true; };
        s->plugin.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        s->plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t) {
            auto& s = self(p);
            // At high rates the cabinet shrinks to fit its 2048-sample delay lines (whirl.c); at
            // 384 kHz the default cabinet is about two thirds of its size.
            if(!(rate >= 8000 && rate <= 384000)) return false;
            if(!s.engine || s.engine->rate != rate)
            {
                if(s.engine && s.engine->contexts.load() > 1) return false; // contexts share one rate
                if(s.engine) --s.engine->contexts;
                s.engine = std::make_shared<Engine>(rate);
                ++s.engine->contexts;
            }
            s.rotors.free(s.fading); s.fading = nullptr;
            s.rotors.free(s.pending.exchange(nullptr));
            s.rotors.free(s.retired.exchange(nullptr));
            auto* r = s.build();
            if(!r) return false;
            s.rotors.free(s.rotor);
            s.rotor = r;
            s.built = s.wanted.load();
            s.speed = int(std::lround(s.values[iSpeed].load()));
            s.fadeLeft = 0; s.quietFor = 0;
            s.fresh = true;
            s.wet = float(s.values[iMix].load());
            s.fadeFrames = std::max<uint32_t>(1, uint32_t(rate * 0.01));
            s.tailFrames = uint32_t(std::ceil(rate * tailSeconds));
            s.active.store(true, std::memory_order_release);
            return true;
        };
        s->plugin.deactivate = [](const clap_plugin_t* p) { self(p).active.store(false, std::memory_order_release); };
        s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
        s->plugin.stop_processing = [](const clap_plugin_t*) {};
        // As a fresh instance given its state: the rotor as built (a rotor built for a configuration
        // changed meanwhile takes over whole), the speed set again, Mix where it is set, the pedals
        // up; asleep, since nothing rings.
        s->plugin.reset = [](const clap_plugin_t* p) {
            auto& s = self(p);
            if(s.rotor) sbf_rotor_reset(s.rotor);
            s.fadeLeft = 0;
            s.fresh = true;
            s.speed = -1;
            s.wheel = s.pedal = 0;
            s.quietFor = s.tailFrames;
            s.tail.store(false);
        };
        s->plugin.process = process;
        s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            if(!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExt;
            if(!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePortsExt;
            if(!std::strcmp(id, CLAP_EXT_PARAMS)) return &paramsExt;
            if(!std::strcmp(id, CLAP_EXT_STATE)) return &stateExt;
            if(!std::strcmp(id, CLAP_EXT_TAIL)) return &tailExt;
            if(!std::strcmp(id, OSCAIRN_EXT_UI)) return &uiExt;
            if(!std::strcmp(id, OSCAIRN_EXT_MEMORY)) return &memoryExt;
            if(!std::strcmp(id, OSCAIRN_EXT_EFFECT_CONTEXTS)) return &contextsExt;
            return nullptr;
        };
        s->plugin.on_main_thread = [](const clap_plugin_t* p) { self(p).service(); };
        return &s->plugin;
    }
}

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "org.oscairn.setbfree.leslie",
    "setBfree Leslie",
    "Oscairn",
    "https://github.com/oscairn/setBfree",
    "",
    "",
    "0.2.1",
    "A rotary speaker: setBfree's Leslie by Fredrik Kilander, Will Panther, Robin Gareus and Ken Restivo",
    features,
};

const clap_plugin_t* create(const clap_host_t* host) { return make(host, nullptr); }
}
