-- iw6-theater: theater mode / demo system / cinematic camera add-on for IW6x (CBServers iw6-mod)
-- Build: tools\premake5 vs2022 (or build.bat), then build build\iw6-theater.sln (Release|x64)

workspace "iw6-theater"
	location "./build"
	objdir "%{wks.location}/obj"
	targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

	configurations { "Debug", "Release" }
	platforms { "x64" }
	architecture "x86_64"

	language "C++"
	cppdialect "C++20"
	systemversion "latest"
	symbols "On"
	staticruntime "On"
	editandcontinue "Off"
	characterset "ASCII"

	flags { "NoIncrementalLink", "MultiProcessorCompile" }

	filter "configurations:Release"
		optimize "Speed"
		defines { "NDEBUG" }
	filter "configurations:Debug"
		optimize "Debug"
		defines { "DEBUG", "_DEBUG" }
	filter {}

	defines { "_WINDOWS", "WIN32", "_CRT_SECURE_NO_WARNINGS", "_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS" }

project "minhook"
	kind "StaticLib"
	language "C"
	warnings "Off"
	files { "deps/minhook/src/**.c", "deps/minhook/src/**.h", "deps/minhook/include/**.h" }
	includedirs { "deps/minhook/include" }

project "zlib"
	kind "StaticLib"
	language "C"
	warnings "Off"
	defines { "ZLIB_CONST", "_CRT_NONSTDC_NO_DEPRECATE" }
	files { "deps/zlib/*.c", "deps/zlib/*.h" }
	includedirs { "deps/zlib" }

project "iw6-theater"
	kind "SharedLib"
	targetname "XINPUT1_3"
	warnings "Extra"

	pchheader "std_include.hpp"
	pchsource "src/std_include.cpp"
	forceincludes { "std_include.hpp" }

	files { "src/**.hpp", "src/**.cpp", "src/exports.def" }
	includedirs { "src", "deps/minhook/include", "deps/zlib" }
	defines { "ZLIB_CONST" }
	links { "minhook", "zlib" }

	-- load close to the game image (0x140000000-0x14B45A000) and next to iw6x.exe (0x150000000)
	-- so rel32 patches into game code can reach the add-on without trampolines
	linkoptions { "/BASE:0x160000000", "/DYNAMICBASE:NO" }

	-- the DLL names its .pdb without the build machine's path
	linkoptions { "/PDBALTPATH:%_PDB%" }
