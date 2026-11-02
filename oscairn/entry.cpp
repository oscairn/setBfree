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

// The bundle: setBfree (the organ) and setBfree Leslie (the rotary speaker as an effect).

#include "leslie.h"
#include "plugin.h"

#include <cstring>

namespace
{
    const clap_plugin_descriptor_t* const descriptors[] = {&organ::descriptor, &leslie::descriptor};
    const clap_plugin_factory_t pluginFactory = {
        [](const clap_plugin_factory_t*) -> uint32_t { return 2; },
        [](const clap_plugin_factory_t*, uint32_t i) { return i < 2 ? descriptors[i] : nullptr; },
        [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
            if(!id) return nullptr;
            if(!std::strcmp(id, organ::descriptor.id)) return organ::create(host);
            if(!std::strcmp(id, leslie::descriptor.id)) return leslie::create(host);
            return nullptr;
        },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char*) { return true; },
    [] {},
    [](const char* id) -> const void* { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &pluginFactory : organ::factory(id); },
};
