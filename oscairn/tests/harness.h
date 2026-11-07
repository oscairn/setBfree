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

// A minimal CLAP host for the organ's tests and tools: one instance, events at sample times,
// rendering in blocks, an allocation counter for the audio thread.

#pragma once
#include "plugin.h"

#include <clap/clap.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace organtest
{
    extern int failures;
#define CHECK(x)                                                                    \
    do                                                                              \
    {                                                                               \
        if(!(x)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++organtest::failures; } \
    } while(0)

    double rms(const std::vector<float>& x, size_t from, size_t to);
    // Frequency from interpolated rising zero crossings over [from, to).
    double frequency(const std::vector<float>& x, size_t from, size_t to, double rate);
    // A Leslie horn's turns per second from a steady tone through it, from `from` on: the
    // strongest periodicity of the tone's pitch (Doppler), measured every 5 ms, 2 to 12 Hz.
    double rotorRate(const std::vector<float>& x, size_t from, double rate);

    struct Host
    {
        clap_host_t host;
        int callbacks = 0;
    };
    extern Host host;

    // Allocations and frees made on a thread while it is inside the plugin's process().
    constexpr bool allocationGuard = ORGAN_ALLOC_GUARD;
    void resetAllocations();
    // Bytes held by allocations made, and not freed, on this thread while tracking.
    void trackBytes(bool on);
    int64_t trackedBytes();
    int allocations();
    int frees();

    struct Output
    {
        std::vector<float> left, right;
        std::vector<std::vector<uint8_t>> events;
    };

    struct Instance
    {
        const clap_plugin_t* p = nullptr;
        double rate;
        uint32_t maxBlock = 4096;
        // An effect's stereo input at each frame since the instance started; none if empty.
        std::function<void(uint64_t frame, float& left, float& right)> input;
        // Given, each process() gets a four-channel ring-out buffer (oscairn.sound-snapshots draft 3):
        // channels 0 and 1 (retiring sounds) are appended here, 2 and 3 (sounds being choked) to chokeOut.
        Output* ringOut = nullptr;
        Output* chokeOut = nullptr;

        explicit Instance(double rate, bool activate = true, const clap_plugin_t* (*make)(const clap_host_t*) = organ::create);
        ~Instance();
        Instance(const Instance&) = delete;

        template<class T> const T* ext(const char* id) const { return static_cast<const T*>(p->get_extension(p, id)); }

        // Events, at frames from the start of the next process or render call.
        Instance& at(uint32_t frame) { time = frame; return *this; }
        Instance& note(uint16_t type, int16_t channel, int16_t key, int32_t id, double velocity = 1.0);
        Instance& param(clap_id id, double value, int16_t channel = -1);
        Instance& midi(uint8_t status, uint8_t data1, uint8_t data2);
        Instance& expression(clap_note_expression id, int16_t channel, int16_t key, int32_t noteId, double value);

        clap_process_status process(uint32_t frames, Output* out = nullptr);
        Output render(uint32_t frames, uint32_t block);
        void service() { p->on_main_thread(p); }
        void reset(); // CLAP reset, counted by the allocation guard as process() is
        std::string save() const;
        bool load(const std::string& data);

    private:
        uint32_t time = 0;
        uint64_t played = 0;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> queued;
        template<class T> void add(const T& e);
    };
}
