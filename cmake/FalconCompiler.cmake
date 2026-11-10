# Compiler settings per toolchain: MSVC on Windows, Clang or GCC on Linux and macOS.

if(MSVC)
  # cef-lite links the static CRT; everything in the process must match.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Release builds keep symbols in .pdb files beside the binaries too (not shipped in games):
  # freeze reports (core/freeze_watch.h) name functions and lines with falcon.pdb. /OPT:REF and
  # /OPT:ICF keep the binaries as they are without /DEBUG.
  # With a compiler cache (sccache: CMAKE_CXX_COMPILER_LAUNCHER, as CI sets it) each object
  # carries its own debug info (/Z7): objects writing into one shared .pdb (/Zi) can't be
  # cached. The linker gathers it into falcon.pdb either way. CEF's wrapper follows. No
  # precompiled headers either (SDL's: sccache can't cache /Yc and /Yu objects).
  if("${CMAKE_CXX_COMPILER_LAUNCHER}$ENV{CMAKE_CXX_COMPILER_LAUNCHER}" MATCHES "sccache")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
    set(CEF_DEBUG_INFO_FLAG "/Z7" CACHE STRING "" FORCE)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
  else()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
  endif()
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
else()
  # Symbols in release builds too (freeze reports name functions); executables find their shared
  # libraries beside them or in the folders the engine loads them from.
  string(APPEND CMAKE_C_FLAGS_RELEASE " -g")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  # No fused multiply-adds the source didn't ask for: a*b+c rounds once as an FMA and twice
  # otherwise, and ARM64 (where every CPU has FMA, and Clang fuses by default) would then
  # compute other bits than x86-64 does. The deterministic math (core/dmath, the simulation
  # and its lockstep across machines) needs the same bits on every OS. (MSVC doesn't contract
  # under /fp:precise.)
  add_compile_options(-ffp-contract=off)
endif()

# falcon_compile_options(<target> [STRICT]): the warnings and source settings Falcon's own code
# builds with. STRICT: everything Falcon writes (MSVC /W4); without it, /W3 (the cooker's
# third-party-heavy sources).
function(falcon_compile_options target)
  cmake_parse_arguments(arg "STRICT" "" "" ${ARGN})
  if(MSVC)
    if(arg_STRICT)
      target_compile_options(${target} PRIVATE /W4)
    else()
      target_compile_options(${target} PRIVATE /W3)
    endif()
    target_compile_options(${target} PRIVATE /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers
      -Wno-sign-compare -Wno-unknown-pragmas)
    if(arg_STRICT)
      target_compile_options(${target} PRIVATE -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    endif()
  endif()
endfunction()
