#pragma once

// iw6-theater: theater mode / demo system / cinematic camera add-on for IW6x (CBServers iw6-mod)
// Licensed under GPL-3.0 (contains code derived from iw6-mod by X Labs/AlterWare and the demo system by Anomaly/Caball009)

#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4127)
#pragma warning(disable: 4244)
#pragma warning(disable: 4458)
#pragma warning(disable: 4702)
#pragma warning(disable: 4996)
#pragma warning(disable: 5054)

#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <d3d11.h>

// min and max is required by gdi, therefore NOMINMAX won't work
#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <MinHook.h>

#pragma warning(pop)
#pragma warning(disable: 4100)
#pragma warning(disable: 4459)

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")

using namespace std::literals;
