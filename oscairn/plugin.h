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

#pragma once
#include <clap/clap.h>

namespace organ
{
    extern const clap_plugin_descriptor_t descriptor;
    const clap_plugin_t* create(const clap_host_t* host);
    const void* factory(const char* id); // the preset discovery factory (entry.cpp has the plugin factory)
    uint64_t voicesFinished(const clap_plugin_t* p); // the sound-context helper's voiceFinished calls so far
}
