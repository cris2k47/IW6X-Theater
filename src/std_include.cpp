#include <std_include.hpp>

// The add-on is linked with a preferred base close to the game image (0x140000000) so that
// rel32 call/jmp patches into game code can reach it directly (see premake5.lua: /BASE, /DYNAMICBASE:NO).
// If the loader still relocates it, utils::hook falls back to near trampolines.
