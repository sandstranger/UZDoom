#pragma once

// Selaco support (GZSelaco's engine features, merged into UZDoom as an additional game).
//
// True while the loaded IWAD is Selaco (IWADINFO: Config = "Selaco"); set by D_DoomMain once the IWAD is known.
// The features Selaco added (new natives, classes, fields, subsystems) are there for every game; where Selaco changed
// how existing engine code behaves, the change is gated with this flag so that Doom, the other games and their mods
// keep UZDoom's behaviour.
extern bool g_SelacoMode;
