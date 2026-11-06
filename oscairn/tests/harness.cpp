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

#include "harness.h"
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace organtest
{
    int failures = 0;

    namespace
    {
        thread_local bool guarded = false, tracking = false;
        thread_local int64_t tracked = 0;
        std::atomic<int> allocated{0}, freed{0};

        const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
        void hostRequest(const clap_host_t*) {}
        void hostCallback(const clap_host_t*) { ++host.callbacks; }

        struct Events
        {
            const std::vector<std::vector<uint8_t>>* in = nullptr;
            std::vector<std::vector<uint8_t>>* out = nullptr;
            static uint32_t size(const clap_input_events_t* l) { return uint32_t(static_cast<Events*>(l->ctx)->in->size()); }
            static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
            {
                return reinterpret_cast<const clap_event_header_t*>((*static_cast<Events*>(l->ctx)->in)[i].data());
            }
            static bool push(const clap_output_events_t* l, const clap_event_header_t* h)
            {
                const bool was = guarded; // the host's storage, not the plugin's
                guarded = false;
                auto* b = reinterpret_cast<const uint8_t*>(h);
                if(auto* out = static_cast<Events*>(l->ctx)->out) out->emplace_back(b, b + h->size);
                guarded = was;
                return true;
            }
        };
    }

    Host host{{CLAP_VERSION_INIT, nullptr, "organ test", "", "", "1", hostExtension, hostRequest, hostRequest, hostCallback}};

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0;
        for(size_t i = from; i < to; ++i) s += double(x[i]) * x[i];
        return std::sqrt(s / double(to - from));
    }
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
    double rotorRate(const std::vector<float>& x, size_t from, double rate)
    {
        std::vector<double> e;
        const size_t w = size_t(rate / 200);
        for(size_t i = from; i + 2 * w <= x.size(); i += w) e.push_back(frequency(x, i, i + 2 * w, rate));
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
        return best;
    }

    void resetAllocations() { allocated = 0; freed = 0; }
    void trackBytes(bool on) { if(on) tracked = 0; tracking = on; }
    int64_t trackedBytes() { return tracked; }
    int allocations() { return allocated; }
    int frees() { return freed; }

    Instance::Instance(double r, bool activate, const clap_plugin_t* (*make)(const clap_host_t*)) : rate(r)
    {
        p = make(&host.host);
        p->init(p);
        if(activate)
        {
            p->activate(p, rate, 1, maxBlock);
            p->start_processing(p);
        }
    }
    Instance::~Instance()
    {
        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }

    template<class T> void Instance::add(const T& e)
    {
        auto* b = reinterpret_cast<const uint8_t*>(&e);
        queued.emplace_back(time, std::vector<uint8_t>(b, b + sizeof(T)));
    }
    Instance& Instance::note(uint16_t type, int16_t channel, int16_t key, int32_t id, double velocity)
    {
        clap_event_note_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
        e.note_id = id; e.port_index = 0; e.channel = channel; e.key = key; e.velocity = velocity;
        add(e);
        return *this;
    }
    Instance& Instance::param(clap_id id, double value, int16_t channel)
    {
        clap_event_param_value_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = id; e.note_id = -1; e.port_index = -1; e.channel = channel; e.key = -1; e.value = value;
        add(e);
        return *this;
    }
    Instance& Instance::midi(uint8_t status, uint8_t data1, uint8_t data2)
    {
        clap_event_midi_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        e.port_index = 0; e.data[0] = status; e.data[1] = data1; e.data[2] = data2;
        add(e);
        return *this;
    }
    Instance& Instance::expression(clap_note_expression id, int16_t channel, int16_t key, int32_t noteId, double value)
    {
        clap_event_note_expression_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
        e.expression_id = id; e.note_id = noteId; e.port_index = 0; e.channel = channel; e.key = key; e.value = value;
        add(e);
        return *this;
    }

    clap_process_status Instance::process(uint32_t frames, Output* out)
    {
        std::stable_sort(queued.begin(), queued.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::vector<uint8_t>> now;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> later;
        for(auto& [t, e] : queued)
        {
            if(t < frames) { reinterpret_cast<clap_event_header_t*>(e.data())->time = t; now.push_back(e); }
            else later.emplace_back(t - frames, e);
        }
        queued = later;
        time = 0;
        std::vector<std::vector<uint8_t>> produced;
        Events events{&now, &produced};
        const clap_input_events_t in{&events, Events::size, Events::get};
        const clap_output_events_t outEvents{&events, Events::push};
        std::vector<float> l(frames, 9.f), r(frames, 9.f); // the plugin must write every sample
        float* channels[2] = {l.data(), r.data()};
        clap_audio_buffer_t buffer{};
        buffer.data32 = channels; buffer.channel_count = 2;
        std::vector<float> il(frames), ir(frames);
        float* inputs[2] = {il.data(), ir.data()};
        clap_audio_buffer_t inBuffer{};
        inBuffer.data32 = inputs; inBuffer.channel_count = 2;
        if(input) for(uint32_t i = 0; i < frames; ++i) input(played + i, il[i], ir[i]);
        played += frames;
        clap_process_t proc{};
        proc.steady_time = -1; proc.frames_count = frames;
        proc.audio_outputs = &buffer; proc.audio_outputs_count = 1;
        if(input) { proc.audio_inputs = &inBuffer; proc.audio_inputs_count = 1; }
        proc.in_events = &in; proc.out_events = &outEvents;
        const uint32_t ringFrames = ringOut ? frames : 0;
        std::vector<float> rl(ringFrames), rr(ringFrames), cl(ringFrames), cr(ringFrames);
        float* ring[4] = {rl.data(), rr.data(), cl.data(), cr.data()};
        clap_audio_buffer_t ringBuffer{};
        ringBuffer.data32 = ring; ringBuffer.channel_count = 4;
        if(ringOut) ext<oscairn_plugin_sound_snapshots_t>(OSCAIRN_EXT_SOUND_SNAPSHOTS)->ring_out(p, &ringBuffer);
        guarded = true;
        const auto status = p->process(p, &proc);
        guarded = false;
        if(ringOut)
        {
            ringOut->left.insert(ringOut->left.end(), rl.begin(), rl.end());
            ringOut->right.insert(ringOut->right.end(), rr.begin(), rr.end());
        }
        if(chokeOut)
        {
            chokeOut->left.insert(chokeOut->left.end(), cl.begin(), cl.end());
            chokeOut->right.insert(chokeOut->right.end(), cr.begin(), cr.end());
        }
        if(out)
        {
            out->left.insert(out->left.end(), l.begin(), l.end());
            out->right.insert(out->right.end(), r.begin(), r.end());
            for(auto& e : produced)
            {
                reinterpret_cast<clap_event_header_t*>(e.data())->time += uint32_t(out->left.size() - frames);
                out->events.push_back(e);
            }
        }
        return status;
    }

    Output Instance::render(uint32_t frames, uint32_t block)
    {
        Output out;
        for(uint32_t done = 0; done < frames;)
        {
            const uint32_t n = std::min(block, frames - done);
            process(n, &out);
            done += n;
        }
        return out;
    }

    std::string Instance::save() const
    {
        auto* state = ext<clap_plugin_state_t>(CLAP_EXT_STATE);
        std::string data;
        clap_ostream_t stream{&data, [](const clap_ostream_t* s, const void* b, uint64_t n) -> int64_t {
                                  static_cast<std::string*>(s->ctx)->append(static_cast<const char*>(b), size_t(n));
                                  return int64_t(n);
                              }};
        return state->save(p, &stream) ? data : std::string();
    }
    bool Instance::load(const std::string& data)
    {
        auto* state = ext<clap_plugin_state_t>(CLAP_EXT_STATE);
        struct Reader { const std::string* data; size_t at; } reader{&data, 0};
        clap_istream_t stream{&reader, [](const clap_istream_t* s, void* b, uint64_t n) -> int64_t {
                                  auto& r = *static_cast<Reader*>(s->ctx);
                                  const size_t k = std::min<size_t>(size_t(n), std::min<size_t>(r.data->size() - r.at, 7)); // short reads
                                  std::memcpy(b, r.data->data() + r.at, k);
                                  r.at += k;
                                  return int64_t(k);
                              }};
        return state->load(p, &stream);
    }
}

// The allocation guard: every allocation and free goes through here; those made while the
// calling thread is inside process() are counted. Sanitizer builds replace the allocator
// themselves and build without it (ORGAN_ALLOC_GUARD=OFF).
#if !ORGAN_ALLOC_GUARD
#elif defined(__APPLE__)
static void* counted(void* p)
{
    if(p && organtest::tracking) organtest::tracked += int64_t(malloc_size(p));
    return p;
}
extern "C" void* malloc(size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    return counted(malloc_zone_malloc(malloc_default_zone(), n));
}
extern "C" void* calloc(size_t c, size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    return counted(malloc_zone_calloc(malloc_default_zone(), c, n));
}
extern "C" void* realloc(void* p, size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    if(!p) return counted(malloc_zone_malloc(malloc_default_zone(), n));
    if(organtest::tracking) organtest::tracked -= int64_t(malloc_size(p));
    return counted(malloc_zone_realloc(malloc_zone_from_ptr(p), p, n));
}
extern "C" void free(void* p)
{
    if(!p) return;
    if(organtest::guarded) ++organtest::freed;
    if(organtest::tracking) organtest::tracked -= int64_t(malloc_size(p));
    if(auto* zone = malloc_zone_from_ptr(p)) malloc_zone_free(zone, p);
}
#elif defined(__GLIBC__)
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void __libc_free(void*);
static void* counted(void* p)
{
    if(p && organtest::tracking) organtest::tracked += int64_t(malloc_usable_size(p));
    return p;
}
extern "C" void* malloc(size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    return counted(__libc_malloc(n));
}
extern "C" void* calloc(size_t c, size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    return counted(__libc_calloc(c, n));
}
extern "C" void* realloc(void* p, size_t n)
{
    if(organtest::guarded) ++organtest::allocated;
    if(p && organtest::tracking) organtest::tracked -= int64_t(malloc_usable_size(p));
    return counted(__libc_realloc(p, n));
}
extern "C" void free(void* p)
{
    if(!p) return;
    if(organtest::guarded) ++organtest::freed;
    if(organtest::tracking) organtest::tracked -= int64_t(malloc_usable_size(p));
    __libc_free(p);
}
#endif
#if ORGAN_ALLOC_GUARD
void* operator new(size_t n)
{
    if(void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif
