# Editor and in-game UI: cef-lite (CEF as an SDL_GPU layer). cef-lite is GPL-3.0, so a build
# with FALCON_UI=ON produces GPL binaries; FALCON_UI=OFF builds the engine without it.
# Needs the CEF binary distribution: powershell -File external/cef-lite/scripts/fetch-cef.ps1
# (Linux, macOS: cmake -DDEP=cef -P scripts/fetch.cmake)
option(FALCON_UI "Editor and in-game UI through cef-lite (GPL-3.0, needs the CEF distribution)" ON)

set(fx_cef_root "${CMAKE_SOURCE_DIR}/external/cef-lite/vendor/cef")
if(FALCON_UI AND NOT IS_DIRECTORY "${fx_cef_root}/cmake")
  message(WARNING "FALCON_UI: no CEF distribution in ${fx_cef_root} (run "
    "external/cef-lite/scripts/fetch-cef.ps1, or cmake -DDEP=cef -P scripts/fetch.cmake); building without UI")
  set(FALCON_UI OFF CACHE BOOL "" FORCE)
endif()

if(FALCON_UI)
  # Our SDL is the in-tree build with cef-lite's external-texture patch (FalconSDL.cmake).
  # cef-lite looks SDL3 up with find_package: answer with a stub, the target already exists.
  set(fx_sdl_stub "${CMAKE_BINARY_DIR}/_deps/sdl3-stub")
  file(WRITE "${fx_sdl_stub}/SDL3Config.cmake" "set(SDL3_FOUND TRUE)\n")
  set(SDL3_DIR "${fx_sdl_stub}" CACHE PATH "" FORCE)
  set(SDL3_EXTERNAL_TEXTURE ON CACHE BOOL "" FORCE)
  set(CEF_LITE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CEF_LITE_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(CEF_ROOT "${fx_cef_root}" CACHE PATH "" FORCE)
  set(SLANGC_EXECUTABLE "${FALCON_SLANG_ROOT}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "" FORCE)
  # CEF ships Debug/ and Release/ binaries and addresses them by $<CONFIGURATION>: the other
  # configurations (profile = RelWithDebInfo) use the Release ones through directory junctions.
  foreach(fx_cfg RelWithDebInfo MinSizeRel)
    if(NOT WIN32)
      if(NOT EXISTS "${fx_cef_root}/${fx_cfg}")
        file(CREATE_LINK Release "${fx_cef_root}/${fx_cfg}" SYMBOLIC)
      endif()
      continue()
    endif()
    if(NOT EXISTS "${fx_cef_root}/${fx_cfg}")
      file(TO_NATIVE_PATH "${fx_cef_root}/${fx_cfg}" fx_link)
      file(TO_NATIVE_PATH "${fx_cef_root}/Release" fx_target)
      execute_process(COMMAND cmd /c mklink /J "${fx_link}" "${fx_target}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE fx_rc)
      if(NOT fx_rc EQUAL 0)
        # A link that went stale (an entry EXISTS can't follow): rmdir takes a directory link
        # away and leaves Release/ alone, del a file one.
        execute_process(COMMAND cmd /c rmdir "${fx_link}" OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND cmd /c del /f /q "${fx_link}" OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND cmd /c mklink /J "${fx_link}" "${fx_target}" OUTPUT_QUIET RESULT_VARIABLE fx_rc)
      endif()
      if(NOT fx_rc EQUAL 0)
        message(FATAL_ERROR "FALCON_UI: could not create the ${fx_cfg} junction in ${fx_cef_root}")
      endif()
    endif()
  endforeach()
  # The CEF runtime (libcef.dll and Chromium's data) in bin/cef/, next to falcon.exe (which loads
  # libcef from there and doubles as CEF's helper process); cef-lite's shaders in bin/shaders/.
  # Linux: libcef.so the same way (found through the rpath). macOS: bin/cef/Chromium Embedded
  # Framework.framework, and the helper bundles beside falcon (falcon_cef_helpers below).
  set(CEF_LITE_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin/cef")
  set(CEF_LITE_SHADER_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/shaders" CACHE PATH "" FORCE)
  # SDL is linked into falcon.exe (FalconSDL.cmake): no SDL3.dll to stage.
  set(SDL3_STATIC ON CACHE BOOL "" FORCE)
  # Our changes to cef-lite, applied to the submodule's working tree once. Each patch names a
  # file and a string it adds there: present means applied (other local edits don't matter).
  #   cef-lite-click-count: pointer click counts reach Chromium (double-click, word and line
  #   selection).
  #   cef-lite-single-exe: the host executable is CEF's helper process too
  #   (cef_lite_run_subprocess, no bootstrap.exe / subprocess DLL); Chromium's resources are
  #   found next to libcef.dll; the runtime is staged by the cef_lite_runtime target.
  #   cef-lite-attach-device: cef_lite_create without a device (Chromium starts while the GPU
  #   device is still being made; cef_lite_attach_device later), and typed characters reach
  #   Chromium on Windows (windows_key_code on char events).
  #   cef-lite-posix: Linux and macOS. main's arguments reach CEF (cef_lite_set_main_args);
  #   Chromium's files per OS (libcef.so and SwiftShader; the framework on macOS, loaded at run
  #   time); the profile under ~/.cache or ~/Library/Caches; text input decoded portably; DXIL
  #   only on Windows; cef_lite_helper, the program macOS's helper bundles run.
  foreach(fx_entry "cef-lite-click-count|include/cef_lite.h|click_count"
                   "cef-lite-single-exe|include/cef_lite.h|cef_lite_run_subprocess"
                   "cef-lite-attach-device|include/cef_lite.h|cef_lite_attach_device"
                   "cef-lite-posix|include/cef_lite.h|cef_lite_set_main_args")
    string(REPLACE "|" ";" fx_entry "${fx_entry}")
    list(GET fx_entry 0 fx_patch)
    list(GET fx_entry 1 fx_marker_file)
    list(GET fx_entry 2 fx_marker)
    set(fx_patch_file "${CMAKE_SOURCE_DIR}/cmake/patches/${fx_patch}.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fx_patch_file}")
    file(READ "${CMAKE_SOURCE_DIR}/external/cef-lite/${fx_marker_file}" fx_marker_text)
    string(FIND "${fx_marker_text}" "${fx_marker}" fx_found)
    if(fx_found EQUAL -1)
      execute_process(COMMAND git apply "${fx_patch_file}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/external/cef-lite" RESULT_VARIABLE fx_rc)
      if(NOT fx_rc EQUAL 0)
        message(FATAL_ERROR "FALCON_UI: ${fx_patch}.patch does not apply to external/cef-lite")
      endif()
      message(STATUS "Falcon: applied ${fx_patch}.patch to cef-lite")
    endif()
  endforeach()
  add_subdirectory("${CMAKE_SOURCE_DIR}/external/cef-lite" "${CMAKE_BINARY_DIR}/_deps/cef-lite" EXCLUDE_FROM_ALL)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CMAKE_BUILD_RPATH "$ORIGIN/cef")
  endif()
  # macOS: Chromium's processes run as Falcon Helper.app and its variants (GPU, Renderer, Plugin,
  # Alerts: Chromium picks one by suffix when Falcon runs bundled), each a copy of
  # cef_lite_helper with its own Info.plist, beside falcon in bin/.
  if(APPLE)
    set(fx_helpers_stamp "${CMAKE_BINARY_DIR}/_deps/falcon-cef-helpers.stamp")
    set(fx_helper_cmds)
    foreach(fx_suffix "" " (GPU)" " (Renderer)" " (Plugin)" " (Alerts)")
      set(fx_name "Falcon Helper${fx_suffix}")
      string(TOLOWER "${fx_suffix}" fx_id)
      string(REGEX REPLACE "[^a-z]" "" fx_id "${fx_id}")
      if(fx_id)
        set(fx_id ".${fx_id}")
      endif()
      set(fx_id "gg.falconnetwork.falcon.helper${fx_id}")
      set(fx_plist "${CMAKE_BINARY_DIR}/_deps/cef-helpers/${fx_name}.plist")
      file(WRITE "${fx_plist}" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleName</key><string>${fx_name}</string>
  <key>CFBundleExecutable</key><string>${fx_name}</string>
  <key>CFBundleIdentifier</key><string>${fx_id}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><string>1</string>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
")
      set(fx_app "${CMAKE_BINARY_DIR}/bin/${fx_name}.app/Contents")
      list(APPEND fx_helper_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory "${fx_app}/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:cef_lite_helper>" "${fx_app}/MacOS/${fx_name}"
        COMMAND ${CMAKE_COMMAND} -E copy "${fx_plist}" "${fx_app}/Info.plist")
    endforeach()
    add_custom_command(OUTPUT "${fx_helpers_stamp}" ${fx_helper_cmds}
      COMMAND ${CMAKE_COMMAND} -E touch "${fx_helpers_stamp}"
      DEPENDS cef_lite_helper
      COMMENT "Making the CEF helper bundles"
      VERBATIM)
    add_custom_target(falcon_cef_helpers ALL DEPENDS "${fx_helpers_stamp}")
  endif()
  # Chromium's renderers run as falcon.exe (--type=renderer). It must not be marked CET
  # compatible (never link it /CETCOMPAT): cef-lite runs Chromium without its sandbox, the
  # sandbox is what turns CET off for renderers, and with CET enforced V8 dies (exit 0xC0000409,
  # fast fail 57) whenever it swaps optimized code under a running function. (CEF's
  # bootstrap.exe, the helper before, is marked CET compatible.)

  # Falcon's own page bundles (ui/editor, ui/hud) next to the game as bin/falcon-ui/.
  file(GLOB_RECURSE fx_ui_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ui/*")
  set(fx_ui_stamp "${CMAKE_BINARY_DIR}/_deps/falcon-ui.stamp")
  add_custom_command(OUTPUT "${fx_ui_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ui" "${CMAKE_BINARY_DIR}/bin/falcon-ui"
    COMMAND ${CMAKE_COMMAND} -E touch "${fx_ui_stamp}"
    DEPENDS ${fx_ui_files}
    COMMENT "Copying UI bundles"
    VERBATIM)
  add_custom_target(falcon_ui_bundles DEPENDS "${fx_ui_stamp}")
endif()
