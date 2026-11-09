#
# mt32-pi for Linux (userspace build)
#
# Builds the portable parts of mt32-pi (synths, MIDI parser, config, ROM and
# SoundFont management) against small Circle/FatFs compatibility shims, with
# ALSA for audio output and MIDI input. The bare-metal Makefile is untouched.
#
#   cmake -S linux -B build-linux && cmake --build build-linux -j
#

cmake_minimum_required(VERSION 3.16)
project(mt32-pi-linux CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Keep assert() enabled like the bare-metal build: shared code relies on side
# effects inside assert() (e.g. CMT32Synth::SwitchROMSet), so never define NDEBUG.
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_C_FLAGS_RELEASE "-O3")

option(MT32PI_ASAN "Build with AddressSanitizer/UBSan" OFF)
if(MT32PI_ASAN)
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address,undefined)
endif()

set(TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(FLUIDSYNTH REQUIRED IMPORTED_TARGET fluidsynth>=2.2)

find_package(Threads REQUIRED)

# libmt32emu from the munt submodule (static, C++ interface only, as on bare metal)
set(libmt32emu_SHARED OFF CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE OFF CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE ON CACHE BOOL "" FORCE)
set(libmt32emu_REQUIRE_ANSI OFF CACHE BOOL "" FORCE)
set(libmt32emu_PKGCONFIG_INSTALL_PREFIX "" CACHE STRING "" FORCE)
add_subdirectory(${TOP}/external/munt/mt32emu ${CMAKE_BINARY_DIR}/mt32emu EXCLUDE_FROM_ALL)
# The bare-metal build compiles mt32emu with -Ofast as well
target_compile_options(mt32emu PRIVATE -Ofast)

add_library(inih STATIC ${TOP}/external/inih/ini.c)
target_include_directories(inih PUBLIC ${TOP}/external/inih)

add_executable(mt32-pi
	# Shared, unmodified (or minimally #ifdef'd) mt32-pi sources
	${TOP}/src/config.cpp
	${TOP}/src/midimonitor.cpp
	${TOP}/src/midiparser.cpp
	${TOP}/src/rommanager.cpp
	${TOP}/src/soundfontmanager.cpp
	${TOP}/src/lcd/ui.cpp
	${TOP}/src/synth/mt32synth.cpp
	${TOP}/src/synth/soundfontsynth.cpp

	# Linux platform layer
	src/alsamidi.cpp
	src/audiooutput.cpp
	src/circle_compat.cpp
	src/ff_posix.cpp
	src/main.cpp
	src/mt32pilinux.cpp
	src/serialmidi.cpp
	src/smf.cpp
)

target_compile_definitions(mt32-pi PRIVATE MT32PI_LINUX)

execute_process(
	COMMAND git describe --tags --dirty --always
	WORKING_DIRECTORY ${TOP}
	OUTPUT_VARIABLE MT32PI_GIT_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(MT32PI_GIT_VERSION)
	target_compile_definitions(mt32-pi PRIVATE MT32_PI_VERSION="${MT32PI_GIT_VERSION}")
endif()

# Compat shims must win over any system header of the same name
target_include_directories(mt32-pi BEFORE PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/compat
	${TOP}/include
	${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_options(mt32-pi PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(mt32-pi PRIVATE
	MT32Emu::mt32emu
	inih
	PkgConfig::ALSA
	PkgConfig::FLUIDSYNTH
	Threads::Threads)

install(TARGETS mt32-pi RUNTIME DESTINATION bin)
