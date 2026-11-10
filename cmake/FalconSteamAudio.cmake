# Valve's Steam Audio (Apache 2.0; HRTF, and later occlusion, reflections and reverb): on when
# scripts/fetch-steamaudio.ps1 (Windows) or `cmake -DDEP=steamaudio -P scripts/fetch.cmake` (Linux,
# macOS) has fetched the SDK's headers and this OS's library into external/steamaudio. On Windows
# phonon.dll is delay-loaded from bin/audio/ (engine/audio/steam_audio.cpp), so a machine without it
# still runs and pans sounds instead; on Linux and macOS libphonon is linked and found in bin/audio/
# through the executables' rpath. THIRDPARTY.md (the SDK's third-party licences) ships next to it.

set(FX_STEAM_AUDIO_ROOT "${CMAKE_SOURCE_DIR}/external/steamaudio")
set(fx_steam_audio_found OFF)
if(WIN32)
  set(FX_PHONON_LIB "${FX_STEAM_AUDIO_ROOT}/lib/phonon.dll")
elseif(APPLE)
  set(FX_PHONON_LIB "${FX_STEAM_AUDIO_ROOT}/lib/libphonon.dylib")
else()
  set(FX_PHONON_LIB "${FX_STEAM_AUDIO_ROOT}/lib/libphonon.so")
endif()
if(EXISTS "${FX_STEAM_AUDIO_ROOT}/include/phonon.h" AND EXISTS "${FX_PHONON_LIB}" AND
   (NOT WIN32 OR EXISTS "${FX_STEAM_AUDIO_ROOT}/lib/phonon.lib"))
  set(fx_steam_audio_found ON)
endif()
option(FALCON_STEAM_AUDIO "Valve's Steam Audio for spatial sound (needs scripts/fetch-steamaudio.ps1)" ${fx_steam_audio_found})
if(FALCON_STEAM_AUDIO AND NOT fx_steam_audio_found)
  message(WARNING "FALCON_STEAM_AUDIO: external/steamaudio is incomplete (run scripts/fetch-steamaudio.ps1); Steam Audio is off")
  set(FALCON_STEAM_AUDIO OFF CACHE BOOL "" FORCE)
endif()
message(STATUS "Falcon: Steam Audio ${FALCON_STEAM_AUDIO}")

if(FALCON_STEAM_AUDIO)
  set(FX_STEAM_AUDIO_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/audio")
  add_custom_target(falcon_steamaudio_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FX_STEAM_AUDIO_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FX_PHONON_LIB}"
      "${FX_STEAM_AUDIO_ROOT}/THIRDPARTY.md"
      "${FX_STEAM_AUDIO_STAGE_DIR}"
    VERBATIM)
  if(NOT WIN32)
    # (executables in bin/ and in tools/ both look in bin/audio/)
    if(APPLE)
      list(APPEND CMAKE_BUILD_RPATH "@executable_path/audio" "@executable_path/../bin/audio")
    else()
      list(APPEND CMAKE_BUILD_RPATH "$ORIGIN/audio" "$ORIGIN/../bin/audio")
    endif()
  endif()
endif()
