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

// setBfree: the tonewheel organ as a CLAP instrument with Oscairn's native
// sound contexts (doc/oscairn-plan.md, section 4).
//
// Each prepared sound owns a whole setBfree engine. A "voice" of the sound-context helper
// is a key held in a sound: its note goes to that sound's engine, and it ends (NOTE_END)
// once the key's release has rendered. A sound whose engine is still sounding (reverb,
// Leslie) stays pinned until it is quiet. Live parameters (drawbars, switches) change the
// engine in place; configuration parameters build a new engine on the main thread, which
// the audio thread crossfades in over 10 ms.

#include "plugin.h"
#include "engine.h"
#include "params.h"

#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/preset-discovery-plugin.hpp>
#include <oscairn-clap/sound-contexts-plugin.hpp>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>
#include <string>

namespace organ
{
namespace
{
    const char* const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, "organ", nullptr};

    constexpr uint32_t contextCapacity = 8, voiceCapacity = 192;
    constexpr uint32_t F = SBF_FRAGMENT;
    constexpr float quietPeak = 1e-5f;       // -100 dBFS
    constexpr uint32_t quietFragments = 16;  // that long below it ends a sound's tail

    const uint32_t iVolume = index("Volume"), iSustain = index("Sustain pedal"), iLeslieSpeed = index("Leslie speed"),
                   iPedalSplit = index("Pedal split"), iLowerSplit = index("Lower split"), iWheelShape = index("Wheel shape"),
                   iLeslie = index("Leslie");

    // A key held in a sound.
    struct Voice : oscairn::contexts::VoiceBase
    {
        bool released = false;
        uint32_t releaseLeft = 0; // frames until the key's release has rendered
    };

    struct Patch
    {
        Values values = defaults;
    };

    // The MIDI value (0..127) setBfree's control function takes for a live parameter.
    uint8_t midiFor(uint32_t i, double v)
    {
        const auto& p = params[i];
        switch(p.map)
        {
            case Map::Drawbar: return uint8_t(127 - std::lround(v * 127.0 / 8.0));
            case Map::Switch: return v >= 0.5 ? 127 : 0;
            case Map::SwitchInv: return v >= 0.5 ? 0 : 127;
            case Map::Vibrato: return uint8_t(std::lround(v) * 23 + 11);
            case Map::Leslie: { const auto k = std::lround(v); return k == 0 ? 64 : k == 1 ? 0 : 127; }
            case Map::Unit: return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 127.0));
            default: return 0;
        }
    }

    const char* const wheelShapeConfigs[] = {
        "",
        "osc.harmonic.1=1.0\nosc.harmonic.3=0.111111111111\nosc.harmonic.5=0.04\nosc.harmonic.7=0.02040816326530612\nosc.harmonic.9=0.012345679012345678\nosc.harmonic.11=0.008264462809917356\n",
        "osc.harmonic.1=1.0\nosc.harmonic.3=0.333333333333\nosc.harmonic.5=0.2\nosc.harmonic.7=0.142857142857\nosc.harmonic.9=0.111111111111\nosc.harmonic.11=0.090909090909\n",
        "osc.harmonic.1=1.0\nosc.harmonic.2=0.5\nosc.harmonic.3=0.333333333333\nosc.harmonic.4=0.25\nosc.harmonic.5=0.2\nosc.harmonic.6=0.166666666666\nosc.harmonic.7=0.142857142857\nosc.harmonic.8=0.125\nosc.harmonic.9=0.111111111111\nosc.harmonic.10=0.1\nosc.harmonic.11=0.090909090909\n",
        "osc.harmonic.1=1.0\nosc.harmonic.2=-0.5\nosc.harmonic.4=0.25\nosc.harmonic.6=0.16666666666666\nosc.harmonic.8=0.0125\nosc.harmonic.10=0.01\n",
    }; // cfg/osc-{triangle,square,sawtooth,xwave}.cfg

    // setBfree configuration text for a sound's configuration parameters (C locale).
    std::string configFor(const Values& v)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(17);
        out << "scanner.modulation.rate=48000\n"; // the vibrato as deep at every rate as at 48 kHz
        for(uint32_t i = 0; i < paramCount; ++i)
        {
            const auto& p = params[i];
            if(p.kind != Kind::Config || p.map == Map::Split) continue;
            if(i == iWheelShape) { out << wheelShapeConfigs[std::lround(v[i])]; continue; }
            if(i == iLeslie) { out << "whirl.bypass=" << (v[i] >= 0.5 ? 0 : 1) << '\n'; continue; }
            out << p.key << '=';
            if(p.map == Map::List && p.configs) out << p.configs[std::lround(v[i])];
            else if(p.map == Map::List || p.map == Map::Integer) out << std::lround(v[i]);
            else out << v[i];
            out << '\n';
        }
        return out.str();
    }

    // What one engine allocates at a rate (measured on macOS with malloc_size: 2.45 MB at
    // 44.1 kHz, 3.14 MB at 96 kHz, 4.71 MB at 176.4 kHz, 7.51 MB at 384 kHz). The tonewheel
    // tables grow with ceil(rate / 48 kHz); the tests keep this at or above what is allocated.
    uint64_t engineBytes(double rate)
    {
        return 1750000 + 750000 * uint64_t(std::ceil(rate / 48000));
    }

    // A setBfree program as a whole sound: defaults, then what the program sets.
    Values programValues(const sbf_program& g)
    {
        Values v = defaults;
        auto set = [&](const char* name, double value) { if(value >= 0) v[index(name)] = value; };
        for(int i = 0; i < 9; ++i)
        {
            v[index("Upper 16'") + i] = g.upper[i];
            if(g.lower[i] >= 0) v[index("Lower 16'") + i] = g.lower[i];
            if(g.pedal[i] >= 0) v[index("Pedal 16'") + i] = g.pedal[i];
        }
        set("Vibrato", g.vibrato); set("Vibrato upper", g.vibratoUpper); set("Vibrato lower", g.vibratoLower);
        set("Percussion", g.percussion); set("Percussion volume", g.percussionSoft);
        set("Percussion decay", g.percussionFast); set("Percussion harmonic", g.percussionSecond);
        set("Overdrive", g.overdrive); set("Leslie speed", g.leslie);
        if(g.reverb >= 0) v[index("Reverb")] = std::round(std::clamp(g.reverb, 0.0, 1.0) * 127) / 127; // its 7-bit steps
        set("Pedal split", g.pedalSplit); set("Lower split", g.lowerSplit);
        auto shift = [&](const char* name, int value) { if(value != -1) v[index(name)] = std::clamp(value, -127, 127); };
        shift("Transpose", g.transpose); shift("Pedal split transpose", g.pedalTranspose);
        shift("Lower split transpose", g.lowerTranspose); shift("Upper split transpose", g.upperTranspose);
        return v;
    }

    struct Fragment
    {
        float l[F]{}, r[F]{};
        uint32_t pos = F; // F: the next sample needs a new fragment
    };

    // Engines retired by the audio thread for the main thread to free. A slot whose faded-out
    // engine finds no room keeps it until there is (retire()), so no engine is ever lost.
    struct RetireRing
    {
        static constexpr uint32_t size = 32;
        std::array<sbf_engine*, size> items{};
        uint32_t capacity = size; // the tests lower it to reach the wait
        std::atomic<uint32_t> head{0}, tail{0};
        uint32_t space() const { return capacity - (head.load(std::memory_order_relaxed) - tail.load(std::memory_order_acquire)); }
        void push(sbf_engine* e) // audio; callers check space()
        {
            const auto h = head.load(std::memory_order_relaxed);
            items[h % capacity] = e;
            head.store(h + 1, std::memory_order_release);
        }
        sbf_engine* pop() // main
        {
            const auto t = tail.load(std::memory_order_relaxed);
            if(t == head.load(std::memory_order_acquire)) return nullptr;
            auto* e = items[t % capacity];
            tail.store(t + 1, std::memory_order_release);
            return e;
        }
    };

    // The engine side of a sound context slot.
    struct Slot
    {
        // Audio thread while the slot is pinned; main thread while it is not, or with processing excluded.
        sbf_engine* engine = nullptr;
        sbf_engine* fading = nullptr;
        uint32_t fadeLeft = 0;
        Fragment cur, old;
        bool ringing = false;
        uint32_t quiet = 0;
        double level = 1.0, exprGain = 1.0;
        uint8_t swell = 127;
        std::array<std::array<uint8_t, 128>, 16> keys{};

        std::atomic<sbf_engine*> pending{nullptr}; // a new engine from the main thread
        std::array<std::atomic<double>, paramCount> shadow{}; // values for the main thread
        std::atomic<uint32_t> wanted{0};
        uint32_t built = 0; // main
        const sbf_engine* latest = nullptr; // main: the last engine built for the slot, alive until it is rebuilt or discarded
        std::string latestConfig;           // and its configuration
    };

    struct Organ : oscairn::contexts::Engine<Organ, Voice, Patch, contextCapacity, voiceCapacity>
    {
        using Base = oscairn::contexts::Engine<Organ, Voice, Patch, contextCapacity, voiceCapacity>;
        static constexpr const char* stateVersion = "oscairn-organ-1";
        static constexpr size_t maxStateBytes = 1 << 18;

        const clap_host_t* host = nullptr;
        double rate = 0; // 0 until activated
        uint32_t fadeFrames = 480;
        double levelGlide = 0.0;
        std::array<Slot, contextCapacity> slots;
        RetireRing retired;
        std::array<int, paramCount> controlId{};
        const int swellId = sbf_control_id("swellpedal1");
        std::array<std::atomic<double>, paramCount> values{}; // get_value: channel 1's sound
        std::array<uint8_t, 16> expression{}, wheel{}, sustainPedal{};
        std::array<bool, voiceCapacity> keyDown{}; // per voice index: its key is down in an engine
        bool wantCallback = false;
        bool active = false; // main thread
        uint64_t finished = 0; // voices ended (voicesFinished, for the tests)
        uint64_t shared = 0;   // engines built from another's tone wheels (enginesShared, for the tests)
        uint32_t engines = 0;  // main: engines alive, in the slots or waiting to be freed (memoryCurrent)
        std::atomic<Values*> presetIn{nullptr}, presetDone{nullptr}; // a loaded preset, main → audio → main

        Organ()
        {
            for(uint32_t i = 0; i < paramCount; ++i)
            {
                values[i] = defaults[i];
                controlId[i] = params[i].kind == Kind::Live ? sbf_control_id(params[i].key) : -1;
            }
            expression.fill(127);
            for(auto& s : slots) for(uint32_t i = 0; i < paramCount; ++i) s.shadow[i] = defaults[i];
        }
        ~Organ()
        {
            delete presetIn.load();
            delete presetDone.load();
            for(auto& s : slots) freeEngines(s);
            while(auto* e = retired.pop()) destroy(e);
        }

        uint32_t slotOf(const Context& c) const { return uint32_t(&c - contexts.data()); }

        // Main thread: an engine for these values at the current rate, for a slot. Its tone wheels
        // are copied from another slot's engine of the same configuration where there is one.
        sbf_engine* build(const Values& v, Slot& into)
        {
            auto config = configFor(v);
            const sbf_engine* like = nullptr;
            for(const auto& s : slots) if(s.latest && s.latestConfig == config) { like = s.latest; break; }
            auto* e = sbf_create_like(rate, config.c_str(), like);
            if(!e) return nullptr;
            ++engines;
            shared += like != nullptr;
            into.latest = e;
            into.latestConfig = std::move(config);
            for(uint32_t i = 0; i < paramCount; ++i)
                if(params[i].kind == Kind::Live) sbf_control(e, controlId[i], midiFor(i, v[i]));
            sbf_keyboard_split(e, int(std::lround(v[iPedalSplit])), int(std::lround(v[iLowerSplit])));
            return e;
        }
        // Main thread: every engine is built and destroyed here, so the count is its own.
        void destroy(sbf_engine* e)
        {
            if(!e) return;
            sbf_destroy(e);
            --engines;
        }
        void freeEngines(Slot& s)
        {
            destroy(s.engine); s.engine = nullptr;
            destroy(s.fading); s.fading = nullptr;
            destroy(s.pending.exchange(nullptr));
        }
        // Main thread, processing excluded: every prepared sound gets a fresh engine.
        void rebuildAll()
        {
            silence();
            for(auto& s : slots) { freeEngines(s); s.latest = nullptr; }
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(!contexts[i].prepared) continue;
                for(uint32_t p = 0; p < paramCount; ++p) s.shadow[p] = contexts[i].patch.values[p];
                s.built = s.wanted.load();
                if(rate > 0) s.engine = build(contexts[i].patch.values, s);
            }
        }
        // Ends every sound's tail and key at once, and puts the pedals back up (activate, reset,
        // snapshot load).
        void silence()
        {
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(s.ringing) { s.ringing = false; contexts[i].users.fetch_sub(1, std::memory_order_release); }
                if(s.engine) sbf_panic(s.engine);
                s.fadeLeft = 0; retire(s);
                s.cur.pos = s.old.pos = F; s.quiet = 0; s.exprGain = 1.0;
                s.swell = 127;
                if(s.engine) sbf_control(s.engine, swellId, s.swell);
                s.level = std::pow(10.0, contexts[i].patch.values[iVolume] / 20.0);
            }
            expression.fill(127); wheel.fill(0); sustainPedal.fill(0);
        }
        // Audio thread (reset): the main thread builds every sound's engine again and the audio
        // thread takes it over as after a configuration change, so the instance then plays as a
        // fresh one given its state would (the organ's wheels and rotors run free, so nothing short
        // of a new engine sounds the same). Until then the old engine plays.
        void renew()
        {
            for(auto& s : slots) if(s.engine) s.wanted.fetch_add(1, std::memory_order_release);
        }

        void wake(uint32_t slot)
        {
            auto& s = slots[slot];
            if(s.ringing) return;
            s.ringing = true; s.quiet = 0;
            contexts[slot].users.fetch_add(1, std::memory_order_relaxed);
        }
        void keyChange(uint32_t slot, int ch, int key, bool on)
        {
            auto& s = slots[slot];
            auto& count = s.keys[ch][key];
            if(on ? count++ != 0 : (count == 0 || --count != 0)) return;
            if(!s.engine) return;
            const uint8_t m[3] = {uint8_t((on ? 0x90 : 0x80) | ch), uint8_t(key), uint8_t(on ? 127 : 0)};
            sbf_midi(s.engine, m, 3);
        }

        // Audio thread: a slot's faded-out engine goes to the main thread once the ring has room.
        void retire(Slot& s)
        {
            if(!s.fading || s.fadeLeft || !retired.space()) return;
            retired.push(s.fading); s.fading = nullptr; wantCallback = true;
        }
        // Audio thread: adopts engines the main thread built for pinned slots.
        void adoptPending()
        {
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(contexts[i].users.load(std::memory_order_acquire) == 0) continue; // unpinned: the main thread's
                retire(s);
                if(s.fading || retired.space() < 2 || !s.pending.load(std::memory_order_relaxed)) continue;
                auto* next = s.pending.exchange(nullptr, std::memory_order_acquire);
                if(!next) continue;
                if(s.engine && s.ringing) { s.fading = s.engine; s.old = s.cur; s.fadeLeft = fadeFrames; }
                else if(s.engine) retired.push(s.engine), wantCallback = true;
                s.engine = next; s.cur.pos = F;
                // The new engine was built from a snapshot of the values: bring it up to date.
                const auto& v = contexts[i].patch.values;
                for(uint32_t p = 0; p < paramCount; ++p)
                    if(params[p].kind == Kind::Live) sbf_control(next, controlId[p], midiFor(p, v[p]));
                sbf_control(next, swellId, s.swell);
                for(int ch = 0; ch < 16; ++ch)
                    for(int k = 0; k < 128; ++k)
                        if(s.keys[ch][k]) { const uint8_t m[3] = {uint8_t(0x90 | ch), uint8_t(k), 127}; sbf_midi(next, m, 3); }
            }
        }
        void applySelections(const clap_output_events_t* out)
        {
            Base::applySelections(out);
            adoptPending();
            if(presetDone.load(std::memory_order_acquire)) return; // the last one is not freed yet
            if(auto* v = presetIn.exchange(nullptr, std::memory_order_acquire))
            {
                for(uint32_t i = 0; i < paramCount; ++i) setFromPerformance(-1, i, (*v)[i]);
                presetDone.store(v, std::memory_order_release);
                wantCallback = true;
            }
        }
        // Main thread: a preset's values for the sound(s) selected now, as a host change on every channel.
        void loadPreset(const Values& v)
        {
            if(!active)
            {
                for(uint32_t i = 0; i < paramCount; ++i) setParam(i, v[i], -1);
                return;
            }
            delete presetIn.exchange(new Values(v), std::memory_order_acq_rel);
            if(host)
                if(auto* hp = static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS))) hp->request_flush(host);
        }

        // Helper hooks.
        bool accepts(const Patch&, const clap_event_note_t&) { return true; }
        void start(Voice& v, Context& c)
        {
            const auto slot = slotOf(c);
            const auto vi = uint32_t(&v - voices.data());
            wake(slot);
            auto& s = slots[slot];
            if(s.engine && s.swell != expression[v.channel])
            { s.swell = expression[v.channel]; sbf_control(s.engine, swellId, s.swell); }
            keyChange(slot, v.channel, v.key, true);
            keyDown[vi] = true;
        }
        // Every way a voice ends (released out, choked, stolen, reset): its key goes up.
        void voiceFinished(Voice& v)
        {
            ++finished;
            const auto vi = uint32_t(&v - voices.data());
            if(keyDown[vi]) { keyDown[vi] = false; keyChange(v.context, v.channel, v.key, false); }
        }
        void release(Voice& v)
        {
            if(v.released) return;
            v.released = true;
            const auto vi = uint32_t(&v - voices.data());
            if(keyDown[vi]) { keyDown[vi] = false; keyChange(v.context, v.channel, v.key, false); }
            // The key-off reaches the tone generator with its next fragment and fades from there.
            const auto& s = slots[v.context];
            v.releaseLeft = s.engine ? F - s.cur.pos + uint32_t(sbf_release_frames(s.engine)) : F;
        }
        void applyMidi(const clap_event_midi_t& e)
        {
            if(e.port_index) return;
            const int ch = e.data[0] & 15, kind = e.data[0] & 0xf0;
            if(kind == 0xb0 && e.data[1] == 1) // mod wheel: the Leslie, like a half-moon switch
            {
                const uint8_t was = wheel[ch], now = e.data[2] & 127;
                wheel[ch] = now;
                if((was < 64) != (now < 64)) setFromPerformance(ch, iLeslieSpeed, now >= 64 ? 2 : 1);
                return;
            }
            if(kind == 0xb0 && e.data[1] == 11) // expression: the swell pedal
            {
                expression[ch] = e.data[2] & 127;
                std::array<bool, contextCapacity> done{};
                for(auto& v : voices) if(v.active && v.channel == ch) done[v.context] = true;
                done[selected[ch]] = true;
                for(uint32_t i = 0; i < contextCapacity; ++i)
                    if(done[i] && slots[i].engine && slots[i].swell != expression[ch])
                    { slots[i].swell = expression[ch]; sbf_control(slots[i].engine, swellId, slots[i].swell); }
                return;
            }
            if(kind == 0xb0 && e.data[1] == 64 && contexts[selected[ch]].patch.values[iSustain] < 0.5) // Leslie toggle
            {
                const uint8_t was = sustainPedal[ch], now = e.data[2] & 127;
                sustainPedal[ch] = now;
                if(was < 64 && now >= 64)
                    setFromPerformance(ch, iLeslieSpeed, contexts[selected[ch]].patch.values[iLeslieSpeed] == 1 ? 2 : 1);
                return;
            }
            Base::applyMidi(e);
        }
        // A parameter changed by playing (mod wheel, pedal): applied like a host change, and reported.
        void setFromPerformance(int ch, uint32_t i, double value)
        {
            setParam(i, value, int16_t(ch));
            if(ch < 0) values[i] = std::clamp(value, params[i].min, params[i].max);
            if(!output) return;
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), eventTime, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = i + 1; ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.value = value;
            output->try_push(output, &ev.header);
        }
        void setParam(uint32_t i, double value, int16_t channel)
        {
            const auto& p = params[i];
            value = std::clamp(value, p.min, p.max);
            if(p.stepped) value = std::round(value);
            if(channel < 0 || channel == 0) values[i] = value;
            std::array<bool, contextCapacity> changed{};
            for(int ch = 0; ch < 16; ++ch)
            {
                if(channel >= 0 && channel != ch) continue;
                const auto slot = selected[ch];
                if(changed[slot]) continue;
                changed[slot] = true;
                auto& c = contexts[slot];
                auto& s = slots[slot];
                if(c.patch.values[i] == value) continue;
                c.patch.values[i] = value;
                s.shadow[i].store(value, std::memory_order_relaxed);
                if(p.kind == Kind::Live && s.engine) sbf_control(s.engine, controlId[i], midiFor(i, value));
                else if(p.kind == Kind::Config) { s.wanted.fetch_add(1, std::memory_order_release); wantCallback = true; }
            }
        }
        void applyParam(const clap_event_header_t* header)
        {
            if(header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) return;
            const auto& e = *reinterpret_cast<const clap_event_param_value_t*>(header);
            if(e.note_id >= 0 || e.key >= 0 || e.port_index > 0) return;
            if(e.param_id < 1 || e.param_id > paramCount || e.channel < -1 || e.channel > 15 || !std::isfinite(e.value)) return;
            setParam(e.param_id - 1, e.value, e.channel);
        }
        void selectedGlobally(Context& next, const clap_output_events_t* out)
        {
            for(uint32_t i = 0; i < paramCount; ++i)
            {
                values[i] = next.patch.values[i];
                if(!out) continue;
                clap_event_param_value_t ev{};
                ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                ev.param_id = i + 1; ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.value = next.patch.values[i];
                out->try_push(out, &ev.header);
            }
        }

        // Renders [from, to): each sounding engine, its crossfade, its level. A retiring sound goes to
        // the ring-out channels the host gave it (oscairn.sound-snapshots draft 3): a sound choked at a
        // second change fades there with its keys, tail and all, since its gain is its keys' amplitude.
        void render(const clap_process_t* process, uint32_t from, uint32_t to)
        {
            float* mainL = process->audio_outputs[0].data32[0];
            float* mainR = process->audio_outputs[0].data32[1];
            float* outL = mainL;
            float* outR = mainR;
            std::fill(mainL + from, mainL + to, 0.f);
            std::fill(mainR + from, mainR + to, 0.f);
            const uint32_t n = to - from;
            // A choked key lets go at once: where other keys hold its sound up, its release is the fade.
            for(auto& v : voices) if(v.active && v.chokeLength && !v.released) release(v);
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(!s.ringing) continue;
                if(!s.engine) { endTail(i); continue; }
                float* const* ring = retiring(i);
                outL = ring ? ring[0] : mainL;
                outR = ring ? ring[1] : mainR;
                // A key's volume expression or choke lowers the sound only where no other key of it is
                // louder: its gain is its keys' largest amplitude, sample by sample while a choke fades.
                std::array<uint16_t, voiceCapacity> own{};
                uint32_t owned = 0;
                bool choking = false;
                for(uint32_t vi = 0; vi < voiceCapacity; ++vi)
                    if(voices[vi].active && voices[vi].context == i) { own[owned++] = uint16_t(vi); choking |= voices[vi].chokeLength != 0; }
                auto gainAt = [&](uint32_t k) {
                    double g = 0;
                    for(uint32_t j = 0; j < owned; ++j) g = std::max(g, amplitude(voices[own[j]], k));
                    return g;
                };
                double g = owned ? gainAt(0) : s.exprGain;
                s.exprGain = owned ? gainAt(n) : g;
                const double target = std::pow(10.0, contexts[i].patch.values[iVolume] / 20.0);
                bool keys = sbf_keys_down(s.engine) > 0;
                for(uint32_t k = 0; k < n; ++k)
                {
                    if(s.cur.pos == F)
                    {
                        sbf_render(s.engine, s.cur.l, s.cur.r);
                        s.cur.pos = 0;
                        float peak = 0;
                        for(uint32_t j = 0; j < F; ++j) peak = std::max({peak, std::fabs(s.cur.l[j]), std::fabs(s.cur.r[j])});
                        keys = sbf_keys_down(s.engine) > 0;
                        s.quiet = (peak < quietPeak && !keys && !s.fadeLeft) ? s.quiet + 1 : 0;
                    }
                    double l = s.cur.l[s.cur.pos], r = s.cur.r[s.cur.pos];
                    ++s.cur.pos;
                    if(s.fadeLeft)
                    {
                        if(s.old.pos == F) { sbf_render(s.fading, s.old.l, s.old.r); s.old.pos = 0; }
                        const double g = double(s.fadeLeft) / double(fadeFrames);
                        l = l * (1 - g) + s.old.l[s.old.pos] * g;
                        r = r * (1 - g) + s.old.r[s.old.pos] * g;
                        ++s.old.pos;
                        if(--s.fadeLeft == 0) retire(s);
                    }
                    s.level += (target - s.level) * levelGlide;
                    if(choking) g = gainAt(k);
                    const double a = s.level * g;
                    outL[from + k] += float(l * a);
                    outR[from + k] += float(r * a);
                }
                // Its tail has ended when it is quiet, or muted: its last key choked or at volume 0.
                if(s.quiet >= quietFragments) endTail(i);
                else if(s.exprGain == 0) endTail(i, true);
            }
            // Keys whose release has rendered end.
            for(auto& v : voices)
            {
                if(!v.active || !v.released) continue;
                if(v.releaseLeft > n) { v.releaseLeft -= n; continue; }
                const auto at = eventTime;
                eventTime = from + (v.releaseLeft ? v.releaseLeft - 1 : 0);
                finishVoice(v);
                eventTime = at;
            }
        }
        void endTail(uint32_t i, bool muted = false)
        {
            auto& s = slots[i];
            for(const auto& v : voices) if(v.active && v.context == i) return;
            // A muted tail is still in the engine: cleared, so the sound's next key starts clean.
            if(muted) { sbf_clear_tail(s.engine); if(s.fading) sbf_clear_tail(s.fading); s.old.pos = F; }
            s.ringing = false; s.quiet = 0; s.cur.pos = F;
            contexts[i].users.fetch_sub(1, std::memory_order_release);
        }

        bool sounding() const
        {
            for(const auto& s : slots) if(s.ringing) return true;
            for(const auto& v : voices) if(v.active) return true;
            return false;
        }

        // Main thread: frees retired engines and builds the ones parameter changes asked for.
        void service()
        {
            delete presetDone.exchange(nullptr, std::memory_order_acq_rel);
            while(auto* e = retired.pop()) destroy(e);
            if(rate <= 0) return;
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                const auto want = s.wanted.load(std::memory_order_acquire);
                if(!contexts[i].prepared || want == s.built) continue;
                s.built = want;
                Values v;
                for(uint32_t p = 0; p < paramCount; ++p) v[p] = s.shadow[p].load(std::memory_order_relaxed);
                if(auto* e = build(v, s)) destroy(s.pending.exchange(e, std::memory_order_release));
            }
        }

        static uint32_t prepare(const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t* supplied, uint32_t count,
                                const char* patch, const oscairn_asset_ref_t* base, char* message, uint32_t capacity)
        {
            auto fail = [&](const char* why) { if(message && capacity) std::snprintf(message, capacity, "%s", why); return CLAP_INVALID_ID; };
            if(patch || base) return fail("the organ takes parameter values, not patch text");
            if(!name || std::strlen(name) > 63) return fail("a sound name of at most 63 bytes is needed");
            if(count > paramCount || (count && !supplied)) return fail("too many parameter values");
            Patch next;
            std::array<bool, paramCount> seen{};
            for(uint32_t i = 0; i < count; ++i)
            {
                const auto& v = supplied[i];
                if(v.id < 1 || v.id > paramCount || seen[v.id - 1] || !std::isfinite(v.value)) return fail("unknown, repeated or non-finite parameter");
                const auto& d = params[v.id - 1];
                if(v.value < d.min || v.value > d.max || (d.stepped && v.value != std::round(v.value))) return fail("parameter value out of range");
                seen[v.id - 1] = true;
                next.values[v.id - 1] = v.value;
            }
            auto& s = self(p);
            auto* c = s.freeSlot();
            if(!c) return fail("all sound slots are in use");
            c->patch = next;
            if(!s.install(*c)) return fail("out of memory");
            return s.publish(*c, name);
        }
        // Main thread: an engine for a new sound's patch (built when active, else at activation).
        bool install(Context& c)
        {
            auto& slot = slots[slotOf(c)];
            for(uint32_t i = 0; i < paramCount; ++i) slot.shadow[i] = c.patch.values[i];
            slot.level = std::pow(10.0, c.patch.values[iVolume] / 20.0);
            if(rate <= 0) return true;
            auto* e = build(c.patch.values, slot);
            if(!e) return false;
            destroy(slot.pending.exchange(e, std::memory_order_release));
            return true;
        }
        // A setBfree program, by its MIDI program number as the preset's load key.
        bool readPreset(uint32_t kind, const char*, const char* key, Patch& patch, std::string& error)
        {
            char* end = nullptr;
            const long pc = key ? std::strtol(key, &end, 10) : -1;
            sbf_program g;
            if(kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !end || end == key || *end || pc < 0 || pc > 127 || !sbf_program_get(int(pc), &g))
            { error = "no such program"; return false; }
            patch.values = programValues(g);
            return true;
        }
        // prepare_preset and a snapshot prepared beside: the helper reads the sound; its engine is built here.
        bool buildPreset(Context& c, std::string& error)
        {
            if(install(c)) return true;
            error = "out of memory";
            return false;
        }
        void discarded(Context& c)
        {
            auto& s = slots[slotOf(c)];
            freeEngines(s); // unpinned: its tail has ended and no key is down
            s.latest = nullptr;
            for(auto& ch : s.keys) ch.fill(0);
        }

        bool writePatch(std::ostream& out, const Patch& patch)
        {
            for(double v : patch.values) out << ' ' << v;
            return true;
        }
        bool readPatch(std::istream& in, Patch& patch)
        {
            std::string line;
            std::getline(in, line);
            std::istringstream fields(line);
            fields.imbue(std::locale::classic());
            uint32_t i = 0;
            for(double v; i < paramCount && fields >> v; ++i)
            {
                const auto& d = params[i];
                if(!std::isfinite(v) || v < d.min || v > d.max || (d.stepped && v != std::round(v))) return false;
                patch.values[i] = v;
            }
            fields >> std::ws;
            return i == paramCount && fields.eof();
        }
        // A snapshot load zeroes every context's pins, the ringing tails' among them: forget those.
        // (A refused state changes nothing and does not come here.)
        void restored()
        {
            for(auto& slot : slots) slot.ringing = false;
            rebuildAll();
            for(uint32_t i = 0; i < paramCount; ++i) values[i] = contexts[selected[0]].patch.values[i];
        }
    };

    Organ& self(const clap_plugin_t* p) { return *static_cast<Organ*>(p->plugin_data); }

    // Parameters.
    uint32_t paramsCount(const clap_plugin_t*) { return paramCount; }
    bool paramsInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
    {
        if(index >= paramCount) return false;
        const auto& p = params[index];
        *info = {};
        info->id = index + 1;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | (p.stepped ? CLAP_PARAM_IS_STEPPED : 0) | (p.labels ? CLAP_PARAM_IS_ENUM : 0);
        info->min_value = p.min; info->max_value = p.max; info->default_value = p.def;
        std::snprintf(info->name, sizeof(info->name), "%s", p.name);
        std::snprintf(info->module, sizeof(info->module), "%s", p.module);
        return true;
    }
    bool paramsValue(const clap_plugin_t* p, clap_id id, double* value)
    {
        if(id < 1 || id > paramCount) return false;
        *value = self(p).values[id - 1];
        return true;
    }

    const char* const noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}

    bool valueToText(const Param& p, double value, char* text, uint32_t size)
    {
        if(!std::isfinite(value) || !size) return false;
        value = std::clamp(value, p.min, p.max);
        const auto k = std::lround(value);
        switch(p.text)
        {
            case Text::List: std::snprintf(text, size, "%s", p.labels[k]); break;
            case Text::Key:
                if(k == 0) std::snprintf(text, size, "Off");
                else std::snprintf(text, size, "%s%ld", noteNames[k % 12], k / 12 - 1);
                break;
            case Text::Percent: std::snprintf(text, size, "%.1f%%", value * 100); break;
            case Text::Decibel:
                if(value <= 0) std::snprintf(text, size, "Off");
                else std::snprintf(text, size, "%.2f dB", 20 * std::log10(value));
                break;
            case Text::Plain:
                if(p.stepped) std::snprintf(text, size, "%ld%s%s", k, *p.unit ? " " : "", p.unit);
                else std::snprintf(text, size, "%.4g%s%s", value, *p.unit ? " " : "", p.unit);
                break;
        }
        return true;
    }
    bool textToValue(const Param& p, const char* text, double* value)
    {
        if(!text) return false;
        double v;
        if(p.text == Text::List)
        {
            for(uint32_t i = 0; p.labels[i]; ++i)
                if(std::strcmp(text, p.labels[i]) == 0) { *value = i; return true; }
            return false;
        }
        if(p.text == Text::Key || p.text == Text::Decibel)
            if(std::strcmp(text, "Off") == 0) { *value = 0; return true; }
        if(p.text == Text::Key)
        {
            for(int n = 11; n >= 0; --n) // longest names first: "C#" before "C"
            {
                const size_t len = std::strlen(noteNames[n]);
                if(std::strncmp(text, noteNames[n], len) != 0) continue;
                char* end = nullptr;
                const long octave = std::strtol(text + len, &end, 10);
                if(end == text + len || *end) return false;
                const long key = (octave + 1) * 12 + n;
                if(key < 1 || key > 127) return false;
                *value = double(key);
                return true;
            }
            return false;
        }
        // Numbers are written in the C locale; read them the same way.
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        if(!(in >> v)) return false;
        std::string unit;
        in >> std::ws;
        std::getline(in, unit);
        if(p.text == Text::Percent) { if(!unit.empty() && unit != "%") return false; v /= 100; }
        else if(p.text == Text::Decibel) { if(!unit.empty() && unit != "dB") return false; v = std::pow(10.0, v / 20); }
        else if(!unit.empty() && unit != p.unit) return false;
        if(!std::isfinite(v)) return false;
        v = std::clamp(v, p.min, p.max);
        *value = p.stepped ? std::round(v) : v;
        return true;
    }

namespace
{
    bool paramsToText(const clap_plugin_t*, clap_id id, double value, char* text, uint32_t size)
    {
        return id >= 1 && id <= paramCount && valueToText(params[id - 1], value, text, size);
    }
    bool paramsFromText(const clap_plugin_t*, clap_id id, const char* text, double* value)
    {
        return id >= 1 && id <= paramCount && textToValue(params[id - 1], text, value);
    }
    void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
    {
        auto& s = self(p);
        s.applySelections(out);
        for(uint32_t i = 0, n = in->size(in); i < n; ++i) s.applyParam(in->get(in, i));
        if(s.wantCallback && s.host) { s.wantCallback = false; s.host->request_callback(s.host); }
    }
    const clap_plugin_params_t paramsExt = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

    const oscairn_plugin_ui_t uiExt = {[](const clap_plugin_t*, clap_id id, oscairn_param_hint_t* hint) {
        if(id < 1 || id > paramCount) return false;
        const auto& p = params[id - 1];
        *hint = {p.widget, p.priority, CLAP_INVALID_ID, p.group, p.role};
        return true;
    }};

    uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
    bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
    {
        if(isInput || index) return false;
        *info = {};
        info->id = 0; info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Main");
        return true;
    }
    const clap_plugin_audio_ports_t audioPortsExt = {audioPortsCount, audioPortsGet};

    uint32_t notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
    bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info)
    {
        if(!isInput || index) return false;
        *info = {};
        info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "Notes");
        return true;
    }
    const clap_plugin_note_ports_t notePortsExt = {notePortsCount, notePortsGet};

    const clap_plugin_state_t stateExt = {Organ::saveSnapshot, Organ::loadSnapshot};
    const oscairn_plugin_sound_contexts_t soundContextsExt = {Organ::capacity, Organ::get, Organ::prepare, Organ::select, Organ::discard, &stateExt, Organ::preparePreset};
    // A snapshot's sounds prepared beside the playing ones (readPatch and buildPreset run on the main thread while rendering);
    // retiring sounds ring out into the host's buffer, and a second change chokes the sounds still retiring from the first.
    const oscairn_plugin_sound_snapshots_t soundSnapshotsExt = {Organ::prepareSnapshot, Organ::setRingOut, Organ::chokeRetiring};

    // oscairn.memory: one engine per prepared sound; the first sound is the context, the rest
    // of the state's sounds are counted as shared.
    bool memoryEstimate(const clap_plugin_t* p, double rate, uint32_t, oscairn_memory_t* out)
    {
        if(rate <= 0) return false;
        uint64_t sounds = 0;
        for(const auto& c : self(p).contexts) sounds += c.prepared;
        out->shared_bytes = sizeof(Organ) + engineBytes(rate) * (sounds - 1);
        out->context_bytes = engineBytes(rate);
        out->asset_bytes = 0;
        return true;
    }
    bool memoryCurrent(const clap_plugin_t* p, oscairn_memory_t* out)
    {
        auto& s = self(p);
        if(s.rate <= 0) return false;
        out->context_bytes = engineBytes(s.rate);
        out->shared_bytes = sizeof(Organ) + engineBytes(s.rate) * (s.engines ? s.engines - 1 : 0);
        out->asset_bytes = 0;
        return true;
    }
    const oscairn_plugin_memory_t memoryExt = {memoryEstimate, memoryCurrent};

    // Presets: setBfree's built-in programs that set drawbars, keyed by MIDI program number.
    const clap_plugin_preset_load_t presetLoadExt = {[](const clap_plugin_t* p, uint32_t kind, const char* location, const char* key) {
        auto& s = self(p);
        Patch patch;
        std::string error;
        auto* hp = s.host ? static_cast<const clap_host_preset_load_t*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD)) : nullptr;
        if(!s.readPreset(kind, location, key, patch, error)) { if(hp) hp->on_error(s.host, kind, location, key, 0, error.c_str()); return false; }
        s.loadPreset(patch.values);
        if(hp) hp->loaded(s.host, kind, location, key);
        return true;
    }};

    clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc)
    {
        auto& s = self(p);
        const auto status = Organ::Base::process(p, proc);
        if(s.wantCallback && s.host) { s.wantCallback = false; s.host->request_callback(s.host); }
        if(status != CLAP_PROCESS_CONTINUE) return status;
        return s.sounding() ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
    }
}

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "org.oscairn.setbfree",
    "setBfree",
    "Oscairn",
    "https://github.com/oscairn/setBfree",
    "",
    "",
    "0.4.2",
    "A tonewheel organ and Leslie: setBfree by Fredrik Kilander, Will Panther, Robin Gareus and Ken Restivo",
    features,
};

uint64_t voicesFinished(const clap_plugin_t* p) { return self(p).finished; }
uint64_t enginesShared(const clap_plugin_t* p) { return self(p).shared; }
void retireCapacity(const clap_plugin_t* p, uint32_t n) { self(p).retired.capacity = std::clamp<uint32_t>(n, 1, RetireRing::size); }

const clap_plugin_t* create(const clap_host_t* host)
{
    auto* s = new Organ;
    s->host = host;
    s->plugin.desc = &descriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = [](const clap_plugin_t*) { return true; };
    s->plugin.destroy = [](const clap_plugin_t* p) { delete &self(p); };
    s->plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t) {
        auto& s = self(p);
        if(rate < 8000 || rate > 768000) return false;
        s.rate = rate;
        s.setSampleRate(rate);
        s.fadeFrames = std::max<uint32_t>(1, uint32_t(rate * 0.01));
        s.levelGlide = 1.0 - std::exp(-1.0 / (rate * 0.005));
        s.resetVoices();
        s.rebuildAll();
        s.active = true;
        return true;
    };
    s->plugin.deactivate = [](const clap_plugin_t* p) {
        auto& s = self(p);
        s.active = false;
        s.resetVoices();
        s.silence();
        if(auto* v = s.presetIn.exchange(nullptr)) { for(uint32_t i = 0; i < paramCount; ++i) s.setParam(i, (*v)[i], -1); delete v; }
    };
    s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
    s->plugin.stop_processing = [](const clap_plugin_t*) {};
    s->plugin.reset = [](const clap_plugin_t* p) {
        auto& s = self(p);
        s.resetVoices();
        s.silence();
        s.renew();
        if(s.host) { s.wantCallback = false; s.host->request_callback(s.host); }
    };
    s->plugin.process = process;
    s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
        if(!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS)) return &soundContextsExt;
        if(!std::strcmp(id, OSCAIRN_EXT_SOUND_SNAPSHOTS)) return &soundSnapshotsExt;
        if(!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExt;
        if(!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePortsExt;
        if(!std::strcmp(id, CLAP_EXT_PARAMS)) return &paramsExt;
        if(!std::strcmp(id, CLAP_EXT_STATE)) return &stateExt;
        if(!std::strcmp(id, OSCAIRN_EXT_UI)) return &uiExt;
        if(!std::strcmp(id, OSCAIRN_EXT_MEMORY)) return &memoryExt;
        if(!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &presetLoadExt;
        return nullptr;
    };
    s->plugin.on_main_thread = [](const clap_plugin_t* p) { self(p).service(); };
    return &s->plugin;
}

namespace
{
    // Preset discovery (the SDK's factory): setBfree's programs inside the plugin, keyed by MIDI
    // program number, so a key keeps its program if the list changes.
    struct Presets
    {
        static constexpr clap_preset_discovery_provider_descriptor_t descriptor = {CLAP_VERSION_INIT, "org.oscairn.setbfree.programs", "setBfree programs", "Oscairn"};
        static constexpr const char* pluginId = "org.oscairn.setbfree";
        static void declare(oscairn::presets::Declarations& d) { d.inside("Factory"); }
        static void list(uint32_t kind, const char*, oscairn::presets::Listing& l)
        {
            if(kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) return;
            for(int pc = 0; pc < 128; ++pc)
            {
                sbf_program g;
                if(sbf_program_get(pc, &g) && !l.add(g.name, std::to_string(pc).c_str())) return;
            }
        }
    };
}

const void* factory(const char* id)
{
    if(!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT)) return &oscairn::presets::Factory<Presets>::factory;
    return nullptr;
}
}
