#pragma once

// Extra IW6 MP symbols used by the theater add-on that are not part of iw6-mod's symbols.hpp.
// Addresses are for iw6mp64_ship.exe 3.15 (SHA256 5A6A109B..., marker 0xDB0A33E7 at 0x140001337).

namespace game
{
	WEAK symbol<void()> R_EndFrame{0, 0x140601AA0};
	WEAK symbol<void(int localClientNum, char* sessionInfo, const netadr_t* addr, const char* mapname, const char* gametype)> CL_ConnectFromParty{0, 0x1402C5700};
	WEAK symbol<void(int localClientNum)> CL_Disconnect{0, 0x1402C6240};
	WEAK symbol<void(int localClientNum, const char* cmd)> CL_ForwardCommandToServer{0, 0x1402C76C0};
	WEAK symbol<void(int localClientNum)> CL_ParseGamestate_unused{0, 0x1402CCC20};
	WEAK symbol<void()> CG_DrawActiveFrame_unused{0, 0x140299760};

	constexpr std::uint64_t VERSION_MARKER_ADDRESS = 0x140001337;
	constexpr std::uint32_t VERSION_MARKER_VALUE = 0xDB0A33E7;
}
