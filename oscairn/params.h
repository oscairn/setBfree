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

// The organ's parameters. A parameter's CLAP ID is its index + 1; IDs never change meaning.
// Three kinds:
//   Live:   a setBfree control function, applied on the audio thread at setBfree's
//           7-bit resolution (the value is rounded to the control's 128 steps).
//   Config: a setBfree configuration key; the sound's engine is rebuilt on the main
//           thread and crossfaded in (doc/oscairn-plan.md, section 4).
//   Plugin: handled by the plugin itself (output level, sustain pedal).

#pragma once
#include <oscairn-clap/ui.h>

#include <array>
#include <cstdint>

namespace organ
{
    enum class Kind : uint8_t { Live, Config, Plugin };
    enum class Map : uint8_t
    {
        Drawbar,  // 0..8 → MIDI 127 - round(v * 127 / 8)
        Switch,   // 0/1 → 0/127
        SwitchInv,// 0/1 → 127/0
        Vibrato,  // 0..5 → v * 23 + 11
        Leslie,   // Stop, Slow, Fast → 64, 0, 127
        Unit,     // 0..1 → round(v * 127)
        Number,   // config: the value
        Integer,  // config: the value as an integer
        List,     // config: labels' config spellings
        Level,    // plugin: output level in dB
        Sustain,  // plugin: what the sustain pedal does
        Split,    // engine keyboard split (0 off, else the first key above the region)
    };
    enum class Text : uint8_t { Plain, Percent, Decibel, List, Key };

    struct Param
    {
        const char* name;
        const char* module;
        double min, max, def;
        bool stepped;
        Kind kind;
        Map map;
        const char* key;      // control function or configuration key
        Text text;
        const char* unit;
        const char* const* labels; // Text::List; Map::List also uses configs
        const char* const* configs;
        uint32_t widget, priority, group, role;
    };

    inline constexpr const char* offOn[] = {"Off", "On", nullptr};
    inline constexpr const char* percVolume[] = {"Normal", "Soft", nullptr};
    inline constexpr const char* percDecay[] = {"Slow", "Fast", nullptr};
    inline constexpr const char* percHarmonic[] = {"Third", "Second", nullptr};
    inline constexpr const char* vibratoTypes[] = {"V1", "C1", "V2", "C2", "V3", "C3", nullptr};
    inline constexpr const char* leslieSpeeds[] = {"Stop", "Slow", "Fast", nullptr};
    inline constexpr const char* temperaments[] = {"Equal", "Gear 60 Hz", "Gear 50 Hz", nullptr};
    inline constexpr const char* temperamentConfigs[] = {"equal", "gear60", "gear50", nullptr};
    inline constexpr const char* envModels[] = {"Click", "Cosine", "Linear", "Shelf", nullptr};
    inline constexpr const char* envModelConfigs[] = {"click", "cosine", "linear", "shelf", nullptr};
    inline constexpr const char* eqMacros[] = {"Spline", "Peak 24", "Peak 46", nullptr};
    inline constexpr const char* eqMacroConfigs[] = {"chspline", "peak24", "peak46", nullptr};
    inline constexpr const char* wheelShapes[] = {"Sine", "Triangle", "Square", "Sawtooth", "X-wave", nullptr};
    inline constexpr const char* filterTypes[] = {"Low pass", "High pass", "Band pass Q", "Band pass 0 dB", "Notch", "All pass", "Peaking", "Low shelf", "High shelf", nullptr};
    inline constexpr const char* sustainModes[] = {"Leslie", "Hold notes", nullptr};

    constexpr uint32_t W_AUTO = OSCAIRN_WIDGET_AUTO, W_SLIDER = OSCAIRN_WIDGET_SLIDER, W_KNOB = OSCAIRN_WIDGET_KNOB,
                       W_BUTTONS = OSCAIRN_WIDGET_BUTTONS, W_SWITCH = OSCAIRN_WIDGET_SWITCH, W_LIST = OSCAIRN_WIDGET_LIST;

#define DRAWBAR(manual, module, footage, fn, def, prio, group) \
    {manual " " footage, module, 0, 8, def, true, Kind::Live, Map::Drawbar, fn, Text::Plain, "", nullptr, nullptr, W_SLIDER, prio, group, 0}
#define LIVE_LIST(name, module, max, def, map, fn, labels, widget, prio) \
    {name, module, 0, max, def, true, Kind::Live, map, fn, Text::List, "", labels, nullptr, widget, prio, 0, 0}
#define LIVE_UNIT(name, module, def, fn, prio) \
    {name, module, 0, 1, def, false, Kind::Live, Map::Unit, fn, Text::Percent, "", nullptr, nullptr, W_KNOB, prio, 0, 0}
#define CFG_NUM(name, module, min, max, def, key, text, unit) \
    {name, module, min, max, def, false, Kind::Config, Map::Number, key, text, unit, nullptr, nullptr, W_AUTO, 0, 0, 0}
#define CFG_INT(name, module, min, max, def, key, unit) \
    {name, module, min, max, def, true, Kind::Config, Map::Integer, key, Text::Plain, unit, nullptr, nullptr, W_AUTO, 0, 0, 0}
#define CFG_LIST(name, module, max, def, key, labels, configs, widget) \
    {name, module, 0, max, def, true, Kind::Config, Map::List, key, Text::List, "", labels, configs, widget, 0, 0, 0}

    // Defaults are setBfree's (doc/oscairn-plan.md decision 8), except where a live control's
    // default lies between its 7-bit steps (section 11).
    inline constexpr Param params[] = {
        DRAWBAR("Upper", "Upper/Drawbars", "16'", "upper.drawbar16", 8, 1, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "5 1/3'", "upper.drawbar513", 8, 2, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "8'", "upper.drawbar8", 6, 3, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "4'", "upper.drawbar4", 0, 4, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "2 2/3'", "upper.drawbar223", 0, 5, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "2'", "upper.drawbar2", 0, 6, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "1 3/5'", "upper.drawbar135", 0, 7, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "1 1/3'", "upper.drawbar113", 0, 8, 1),
        DRAWBAR("Upper", "Upper/Drawbars", "1'", "upper.drawbar1", 0, 9, 1),
        DRAWBAR("Lower", "Lower/Drawbars", "16'", "lower.drawbar16", 0, 20, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "5 1/3'", "lower.drawbar513", 0, 21, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "8'", "lower.drawbar8", 0, 22, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "4'", "lower.drawbar4", 0, 23, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "2 2/3'", "lower.drawbar223", 0, 24, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "2'", "lower.drawbar2", 0, 25, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "1 3/5'", "lower.drawbar135", 0, 26, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "1 1/3'", "lower.drawbar113", 0, 27, 2),
        DRAWBAR("Lower", "Lower/Drawbars", "1'", "lower.drawbar1", 0, 28, 2),
        DRAWBAR("Pedal", "Pedal/Drawbars", "16'", "pedal.drawbar16", 0, 30, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "5 1/3'", "pedal.drawbar513", 0, 31, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "8'", "pedal.drawbar8", 0, 32, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "4'", "pedal.drawbar4", 0, 33, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "2 2/3'", "pedal.drawbar223", 0, 34, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "2'", "pedal.drawbar2", 0, 35, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "1 3/5'", "pedal.drawbar135", 0, 36, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "1 1/3'", "pedal.drawbar113", 0, 37, 3),
        DRAWBAR("Pedal", "Pedal/Drawbars", "1'", "pedal.drawbar1", 0, 38, 3),

        LIVE_LIST("Percussion", "Percussion", 1, 0, Map::Switch, "percussion.enable", offOn, W_SWITCH, 11),
        LIVE_LIST("Percussion volume", "Percussion", 1, 0, Map::Switch, "percussion.volume", percVolume, W_BUTTONS, 12),
        LIVE_LIST("Percussion decay", "Percussion", 1, 0, Map::Switch, "percussion.decay", percDecay, W_BUTTONS, 13),
        LIVE_LIST("Percussion harmonic", "Percussion", 1, 0, Map::Switch, "percussion.harmonic", percHarmonic, W_BUTTONS, 14),

        LIVE_LIST("Vibrato", "Vibrato", 5, 5, Map::Vibrato, "vibrato.knob", vibratoTypes, W_BUTTONS, 15),
        LIVE_LIST("Vibrato upper", "Vibrato", 1, 0, Map::Switch, "vibrato.upper", offOn, W_SWITCH, 16),
        LIVE_LIST("Vibrato lower", "Vibrato", 1, 0, Map::Switch, "vibrato.lower", offOn, W_SWITCH, 17),

        LIVE_LIST("Leslie speed", "Leslie", 2, 1, Map::Leslie, "rotary.speed-preset", leslieSpeeds, W_BUTTONS, 10),

        LIVE_LIST("Overdrive", "Overdrive", 1, 0, Map::Switch, "overdrive.enable", offOn, W_SWITCH, 18),
        LIVE_UNIT("Overdrive character", "Overdrive", 0, "overdrive.character", 19),
        LIVE_UNIT("Overdrive drive", "Overdrive", 45.0 / 127, "overdrive.inputgain", 29), // the preamp starts at 3.5675, step 45 is 3.5636
        CFG_NUM("Overdrive output", "Overdrive", 0.01, 10, 0.8795, "overdrive.outputgain", Text::Decibel, "dB"), // initPreamp's, not its doc's 0.07873

        LIVE_UNIT("Reverb", "Reverb", 13.0 / 127, "reverb.mix", 39),

        {"Volume", "Output", -40, 12, 0, false, Kind::Plugin, Map::Level, nullptr, Text::Plain, "dB", nullptr, nullptr, W_KNOB, 40, 0, OSCAIRN_ROLE_LEVEL},
        {"Sustain pedal", "Keyboard", 0, 1, 0, true, Kind::Plugin, Map::Sustain, nullptr, Text::List, "", sustainModes, nullptr, W_BUTTONS, 0, 0, 0},
        CFG_INT("Transpose", "Keyboard", -48, 48, 0, "midi.transpose", "semitones"),
        {"Pedal split", "Keyboard", 0, 127, 0, true, Kind::Config, Map::Split, nullptr, Text::Key, "", nullptr, nullptr, W_AUTO, 0, 0, 0},
        {"Lower split", "Keyboard", 0, 127, 0, true, Kind::Config, Map::Split, nullptr, Text::Key, "", nullptr, nullptr, W_AUTO, 0, 0, 0},
        CFG_INT("Pedal split transpose", "Keyboard", -48, 48, 0, "midi.pedals.transpose.split", "semitones"),
        CFG_INT("Lower split transpose", "Keyboard", -48, 48, 0, "midi.lower.transpose.split", "semitones"),
        CFG_INT("Upper split transpose", "Keyboard", -48, 48, 0, "midi.upper.transpose.split", "semitones"),

        CFG_NUM("Tuning", "Tonewheels", 220, 880, 440, "osc.tuning", Text::Plain, "Hz"),
        CFG_LIST("Temperament", "Tonewheels", 2, 1, "osc.temperament", temperaments, temperamentConfigs, W_BUTTONS),
        CFG_LIST("Wheel shape", "Tonewheels", 4, 0, nullptr, wheelShapes, nullptr, W_LIST),
        CFG_LIST("Wheel EQ", "Tonewheels", 2, 0, "osc.eq.macro", eqMacros, eqMacroConfigs, W_BUTTONS),
        CFG_NUM("Wheel EQ low level", "Tonewheels", 0, 2, 1, "osc.eq.p1y", Text::Plain, ""),
        CFG_NUM("Wheel EQ low slope", "Tonewheels", -3, 3, 0, "osc.eq.r1y", Text::Plain, ""),
        CFG_NUM("Wheel EQ high level", "Tonewheels", 0, 2, 1, "osc.eq.p4y", Text::Plain, ""),
        CFG_NUM("Wheel EQ high slope", "Tonewheels", -3, 3, 0, "osc.eq.r4y", Text::Plain, ""),
        CFG_NUM("Compartment crosstalk", "Tonewheels/Leakage", 0, 0.5, 0.01, "osc.compartment-crosstalk", Text::Decibel, "dB"),
        CFG_NUM("Transformer crosstalk", "Tonewheels/Leakage", 0, 0.5, 0, "osc.transformer-crosstalk", Text::Decibel, "dB"),
        CFG_NUM("Terminal strip crosstalk", "Tonewheels/Leakage", 0, 0.5, 0.01, "osc.terminalstrip-crosstalk", Text::Decibel, "dB"),
        CFG_NUM("Wiring crosstalk", "Tonewheels/Leakage", 0, 0.5, 0.01, "osc.wiring-crosstalk", Text::Decibel, "dB"),
        CFG_NUM("Key click", "Tonewheels/Key click", 0, 1, 0.5, "osc.attack.click.level", Text::Percent, ""),
        CFG_NUM("Release click", "Tonewheels/Key click", 0, 1, 0.25, "osc.release.click.level", Text::Percent, ""),
        CFG_NUM("Click shortest", "Tonewheels/Key click", 0, 1, 0.125, "osc.attack.click.minlength", Text::Percent, ""),
        CFG_NUM("Click longest", "Tonewheels/Key click", 0, 1, 0.625, "osc.attack.click.maxlength", Text::Percent, ""),
        CFG_LIST("Attack model", "Tonewheels/Key click", 3, 0, "osc.attack.model", envModels, envModelConfigs, W_BUTTONS),
        CFG_LIST("Release model", "Tonewheels/Key click", 3, 2, "osc.release.model", envModels, envModelConfigs, W_BUTTONS),

        CFG_NUM("Percussion fast decay", "Percussion/Voicing", 0.1, 10, 1, "osc.perc.fast", Text::Plain, "s"),
        CFG_NUM("Percussion slow decay", "Percussion/Voicing", 0.1, 10, 4, "osc.perc.slow", Text::Plain, "s"),
        CFG_NUM("Percussion normal level", "Percussion/Voicing", 0, 1, 1, "osc.perc.normal", Text::Decibel, "dB"),
        CFG_NUM("Percussion soft level", "Percussion/Voicing", 0, 0.89125, 0.5012, "osc.perc.soft", Text::Decibel, "dB"),
        CFG_NUM("Percussion gain", "Percussion/Voicing", 0, 22, 11, "osc.perc.gain", Text::Plain, ""),

        CFG_NUM("Scanner rate", "Vibrato/Scanner", 4, 22, 7.25, "scanner.hz", Text::Plain, "Hz"),
        CFG_NUM("V1 depth", "Vibrato/Scanner", 0, 12, 3, "scanner.modulation.v1", Text::Plain, ""),
        CFG_NUM("V2 depth", "Vibrato/Scanner", 0, 12, 6, "scanner.modulation.v2", Text::Plain, ""),
        CFG_NUM("V3 depth", "Vibrato/Scanner", 0, 12, 9, "scanner.modulation.v3", Text::Plain, ""),

        CFG_LIST("Leslie", "Leslie", 1, 1, nullptr, offOn, nullptr, W_SWITCH),
        CFG_NUM("Horn slow speed", "Leslie/Rotors", 5, 200, 40.32, "whirl.horn.slowrpm", Text::Plain, "RPM"),
        CFG_NUM("Horn fast speed", "Leslie/Rotors", 100, 900, 423.36, "whirl.horn.fastrpm", Text::Plain, "RPM"),
        CFG_NUM("Horn acceleration", "Leslie/Rotors", 0.05, 2, 0.161, "whirl.horn.acceleration", Text::Plain, "s"),
        CFG_NUM("Horn deceleration", "Leslie/Rotors", 0.05, 2, 0.321, "whirl.horn.deceleration", Text::Plain, "s"),
        CFG_NUM("Horn brake position", "Leslie/Rotors", 0, 1, 0, "whirl.horn.brakepos", Text::Plain, ""),
        CFG_NUM("Drum slow speed", "Leslie/Rotors", 5, 100, 36, "whirl.drum.slowrpm", Text::Plain, "RPM"),
        CFG_NUM("Drum fast speed", "Leslie/Rotors", 60, 600, 357.3, "whirl.drum.fastrpm", Text::Plain, "RPM"),
        CFG_NUM("Drum acceleration", "Leslie/Rotors", 0.5, 10, 4.127, "whirl.drum.acceleration", Text::Plain, "s"),
        CFG_NUM("Drum deceleration", "Leslie/Rotors", 0.5, 10, 1.371, "whirl.drum.deceleration", Text::Plain, "s"),
        CFG_NUM("Drum brake position", "Leslie/Rotors", 0, 1, 0, "whirl.drum.brakepos", Text::Plain, ""),
        CFG_NUM("Horn radius", "Leslie/Cabinet", 9, 50, 19.2, "whirl.horn.radius", Text::Plain, "cm"),
        CFG_NUM("Drum radius", "Leslie/Cabinet", 9, 50, 22, "whirl.drum.radius", Text::Plain, "cm"),
        CFG_NUM("Mic distance", "Leslie/Cabinet", 9, 100, 42, "whirl.mic.distance", Text::Plain, "cm"),
        CFG_NUM("Horn mic angle", "Leslie/Cabinet", 0, 180, 180, "whirl.horn.mic.angle", Text::Plain, "deg"),
        CFG_NUM("Horn width", "Leslie/Cabinet", -1, 1, 0, "whirl.horn.width", Text::Plain, ""),
        CFG_NUM("Drum width", "Leslie/Cabinet", -1, 1, 0, "whirl.drum.width", Text::Plain, ""),
        CFG_NUM("Horn level", "Leslie/Cabinet", 0, 1, 0.7, "whirl.horn.level", Text::Decibel, "dB"),
        CFG_NUM("Horn leakage", "Leslie/Cabinet", 0, 1, 0.15, "whirl.horn.leak", Text::Decibel, "dB"),
        CFG_LIST("Horn filter A type", "Leslie/Filters", 8, 0, "whirl.horn.filter.a.type", filterTypes, nullptr, W_LIST),
        CFG_NUM("Horn filter A frequency", "Leslie/Filters", 250, 8000, 4500, "whirl.horn.filter.a.hz", Text::Plain, "Hz"),
        CFG_NUM("Horn filter A Q", "Leslie/Filters", 0.01, 6, 2.7456, "whirl.horn.filter.a.q", Text::Plain, ""),
        CFG_NUM("Horn filter A gain", "Leslie/Filters", -48, 48, -30, "whirl.horn.filter.a.gain", Text::Plain, "dB"),
        CFG_LIST("Horn filter B type", "Leslie/Filters", 8, 7, "whirl.horn.filter.b.type", filterTypes, nullptr, W_LIST),
        CFG_NUM("Horn filter B frequency", "Leslie/Filters", 250, 8000, 300, "whirl.horn.filter.b.hz", Text::Plain, "Hz"),
        CFG_NUM("Horn filter B Q", "Leslie/Filters", 0.01, 6, 1, "whirl.horn.filter.b.q", Text::Plain, ""),
        CFG_NUM("Horn filter B gain", "Leslie/Filters", -48, 48, -30, "whirl.horn.filter.b.gain", Text::Plain, "dB"),
        CFG_LIST("Drum filter type", "Leslie/Filters", 8, 8, "whirl.drum.filter.type", filterTypes, nullptr, W_LIST),
        CFG_NUM("Drum filter frequency", "Leslie/Filters", 20, 8000, 811.9695, "whirl.drum.filter.hz", Text::Plain, "Hz"),
        CFG_NUM("Drum filter Q", "Leslie/Filters", 0.1, 6, 1.6016, "whirl.drum.filter.q", Text::Plain, ""),
        CFG_NUM("Drum filter gain", "Leslie/Filters", -48, 48, -38.9291, "whirl.drum.filter.gain", Text::Plain, "dB"),
    };
#undef DRAWBAR
#undef LIVE_LIST
#undef LIVE_UNIT
#undef CFG_NUM
#undef CFG_INT
#undef CFG_LIST

    constexpr uint32_t paramCount = sizeof(params) / sizeof(params[0]);
    using Values = std::array<double, paramCount>;

    constexpr Values defaults = [] {
        Values v{};
        for(uint32_t i = 0; i < paramCount; ++i) v[i] = params[i].def;
        return v;
    }();

    // Index of a parameter by name, at compile time.
    constexpr uint32_t index(const char* name)
    {
        for(uint32_t i = 0; i < paramCount; ++i)
        {
            const char *a = params[i].name, *b = name;
            while(*a && *a == *b) { ++a; ++b; }
            if(*a == *b) return i;
        }
        return paramCount;
    }
}
