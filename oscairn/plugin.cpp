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

// Oscairn Organ: setBfree's tonewheel organ as a CLAP instrument with Oscairn's native
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

    struct Fragment
    {
        float l[F]{}, r[F]{};
        uint32_t pos = F; // F: the next sample needs a new fragment
    };

    // Engines retired by the audio thread for the main thread to free.
    struct RetireRing
    {
        static constexpr uint32_t capacity = 32;
        std::array<sbf_engine*, capacity> items{};
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
        std::array<uint16_t, voiceCapacity> keyOf{}; // slot << 11 | channel << 7 | key
        bool wantCallback = false;

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
            for(auto& s : slots) freeEngines(s);
            while(auto* e = retired.pop()) sbf_destroy(e);
        }

        uint32_t slotOf(const Context& c) const { return uint32_t(&c - contexts.data()); }

        // Main thread: an engine for these values at the current rate.
        sbf_engine* build(const Values& v) const
        {
            auto* e = sbf_create(rate, configFor(v).c_str());
            if(!e) return nullptr;
            for(uint32_t i = 0; i < paramCount; ++i)
                if(params[i].kind == Kind::Live) sbf_control(e, controlId[i], midiFor(i, v[i]));
            sbf_keyboard_split(e, int(std::lround(v[iPedalSplit])), int(std::lround(v[iLowerSplit])));
            return e;
        }
        static void freeEngines(Slot& s)
        {
            sbf_destroy(s.engine); s.engine = nullptr;
            sbf_destroy(s.fading); s.fading = nullptr;
            sbf_destroy(s.pending.exchange(nullptr));
        }
        // Main thread, processing excluded: every prepared sound gets a fresh engine.
        void rebuildAll()
        {
            silence();
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                freeEngines(s);
                if(!contexts[i].prepared) continue;
                for(uint32_t p = 0; p < paramCount; ++p) s.shadow[p] = contexts[i].patch.values[p];
                s.built = s.wanted.load();
                if(rate > 0) s.engine = build(contexts[i].patch.values);
                s.level = std::pow(10.0, contexts[i].patch.values[iVolume] / 20.0);
            }
        }
        // Ends every sound's tail and key at once (activate, reset, snapshot load).
        void silence()
        {
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(s.ringing) { s.ringing = false; contexts[i].users.fetch_sub(1, std::memory_order_release); }
                if(s.engine) sbf_panic(s.engine);
                if(s.fading) { if(retired.space()) retired.push(s.fading); else sbf_panic(s.fading); s.fading = nullptr; }
                s.cur.pos = s.old.pos = F; s.quiet = 0; s.exprGain = 1.0;
                for(auto& ch : s.keys) ch.fill(0);
            }
            keyDown.fill(false);
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

        // Audio thread: adopts engines the main thread built for pinned slots.
        void adoptPending()
        {
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(contexts[i].users.load(std::memory_order_acquire) == 0) continue;
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
        }

        // Helper hooks.
        bool accepts(const Patch&, const clap_event_note_t&) { return true; }
        void start(Voice& v, Context& c)
        {
            const auto slot = slotOf(c);
            const auto vi = uint32_t(&v - voices.data());
            if(keyDown[vi]) { const auto k = keyOf[vi]; keyChange(k >> 11, (k >> 7) & 15, k & 127, false); }
            wake(slot);
            auto& s = slots[slot];
            if(s.engine && s.swell != expression[v.channel])
            { s.swell = expression[v.channel]; sbf_control(s.engine, swellId, s.swell); }
            keyChange(slot, v.channel, v.key, true);
            keyDown[vi] = true; keyOf[vi] = uint16_t(slot << 11 | v.channel << 7 | v.key);
        }
        void release(Voice& v)
        {
            if(v.released) return;
            v.released = true;
            const auto vi = uint32_t(&v - voices.data());
            if(keyDown[vi]) { keyDown[vi] = false; keyChange(v.context, v.channel, v.key, false); }
            // The key-off reaches the tone generator with its next fragment and fades over that one.
            const auto& s = slots[v.context];
            v.releaseLeft = (s.engine ? F - s.cur.pos : 0) + F;
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

        // Renders [from, to): each sounding engine, its crossfade, its level.
        void render(const clap_process_t* process, uint32_t from, uint32_t to)
        {
            float* outL = process->audio_outputs[0].data32[0];
            float* outR = process->audio_outputs[0].data32[1];
            std::fill(outL + from, outL + to, 0.f);
            std::fill(outR + from, outR + to, 0.f);
            const uint32_t n = to - from;
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                if(!s.ringing) continue;
                if(!s.engine) { endTail(i); continue; }
                // A key's volume expression or choke lowers the sound only where no other key of it is louder.
                double g0 = -1, g1 = -1;
                for(const auto& v : voices)
                    if(v.active && v.context == i) { g0 = std::max(g0, amplitude(v, 0)); g1 = std::max(g1, amplitude(v, n)); }
                if(g0 < 0) g0 = g1 = s.exprGain;
                s.exprGain = g1;
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
                        s.quiet = (peak < quietPeak && !keys && !s.fading) ? s.quiet + 1 : 0;
                    }
                    double l = s.cur.l[s.cur.pos], r = s.cur.r[s.cur.pos];
                    ++s.cur.pos;
                    if(s.fading)
                    {
                        if(s.old.pos == F) { sbf_render(s.fading, s.old.l, s.old.r); s.old.pos = 0; }
                        const double g = double(s.fadeLeft) / double(fadeFrames);
                        l = l * (1 - g) + s.old.l[s.old.pos] * g;
                        r = r * (1 - g) + s.old.r[s.old.pos] * g;
                        ++s.old.pos;
                        if(--s.fadeLeft == 0) { retired.push(s.fading); s.fading = nullptr; wantCallback = true; }
                    }
                    s.level += (target - s.level) * levelGlide;
                    const double g = s.level * (g0 + (g1 - g0) * double(k) / double(n));
                    outL[from + k] += float(l * g);
                    outR[from + k] += float(r * g);
                }
                if(s.quiet >= quietFragments) endTail(i);
            }
            // Keys whose release has rendered end; keys of voices ended elsewhere (a steal) are let go.
            for(uint32_t vi = 0; vi < voiceCapacity; ++vi)
            {
                auto& v = voices[vi];
                if(!v.active)
                {
                    if(keyDown[vi]) { keyDown[vi] = false; const auto k = keyOf[vi]; keyChange(k >> 11, (k >> 7) & 15, k & 127, false); }
                    continue;
                }
                if(!v.released) continue;
                if(v.releaseLeft > n) { v.releaseLeft -= n; continue; }
                const auto at = eventTime;
                eventTime = from + (v.releaseLeft ? v.releaseLeft - 1 : 0);
                finishVoice(v);
                eventTime = at;
            }
        }
        void endTail(uint32_t i)
        {
            auto& s = slots[i];
            for(const auto& v : voices) if(v.active && v.context == i) return;
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
            while(auto* e = retired.pop()) sbf_destroy(e);
            if(rate <= 0) return;
            for(uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto& s = slots[i];
                const auto want = s.wanted.load(std::memory_order_acquire);
                if(!contexts[i].prepared || want == s.built) continue;
                s.built = want;
                Values v;
                for(uint32_t p = 0; p < paramCount; ++p) v[p] = s.shadow[p].load(std::memory_order_relaxed);
                if(auto* e = build(v)) sbf_destroy(s.pending.exchange(e, std::memory_order_release));
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
            auto& slot = s.slots[s.slotOf(*c)];
            for(uint32_t i = 0; i < paramCount; ++i) slot.shadow[i] = next.values[i];
            slot.level = std::pow(10.0, next.values[iVolume] / 20.0);
            if(s.rate > 0)
            {
                auto* e = s.build(next.values);
                if(!e) return fail("out of memory");
                sbf_destroy(slot.pending.exchange(e, std::memory_order_release));
            }
            c->patch = next;
            return s.publish(*c, name);
        }
        void discarded(Context& c)
        {
            auto& s = slots[slotOf(c)];
            freeEngines(s); // unpinned: its tail has ended and no key is down
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
        void restored()
        {
            // loadSnapshot zeroed every context's pins, the tails' among them.
            for(auto& s : slots) s.ringing = false;
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

    bool paramsToText(const clap_plugin_t*, clap_id id, double value, char* text, uint32_t size)
    {
        if(id < 1 || id > paramCount || !std::isfinite(value) || !size) return false;
        const auto& p = params[id - 1];
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
    bool paramsFromText(const clap_plugin_t*, clap_id id, const char* text, double* value)
    {
        if(id < 1 || id > paramCount || !text) return false;
        const auto& p = params[id - 1];
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
    const oscairn_plugin_sound_contexts_t soundContextsExt = {Organ::capacity, Organ::get, Organ::prepare, Organ::select, Organ::discard, &stateExt};

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
    "org.oscairn.organ",
    "Oscairn Organ",
    "Oscairn",
    "https://github.com/oscairn/setBfree",
    "",
    "",
    "0.1.0",
    "A tonewheel organ and Leslie: setBfree by Fredrik Kilander, Will Panther, Robin Gareus and Ken Restivo",
    features,
};

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
        return true;
    };
    s->plugin.deactivate = [](const clap_plugin_t* p) { self(p).resetVoices(); self(p).silence(); };
    s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
    s->plugin.stop_processing = [](const clap_plugin_t*) {};
    s->plugin.reset = [](const clap_plugin_t* p) { self(p).resetVoices(); self(p).silence(); };
    s->plugin.process = process;
    s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
        if(!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS)) return &soundContextsExt;
        if(!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExt;
        if(!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePortsExt;
        if(!std::strcmp(id, CLAP_EXT_PARAMS)) return &paramsExt;
        if(!std::strcmp(id, CLAP_EXT_STATE)) return &stateExt;
        if(!std::strcmp(id, OSCAIRN_EXT_UI)) return &uiExt;
        return nullptr;
    };
    s->plugin.on_main_thread = [](const clap_plugin_t* p) { self(p).service(); };
    return &s->plugin;
}

namespace
{
    uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }
    const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t i) { return i ? nullptr : &descriptor; }
    const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
    {
        return id && !std::strcmp(id, descriptor.id) ? create(host) : nullptr;
    }
    const clap_plugin_factory_t pluginFactory = {factoryCount, factoryDescriptor, factoryCreate};
}

const void* factory(const char* id)
{
    if(!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &pluginFactory;
    return nullptr;
}
}
