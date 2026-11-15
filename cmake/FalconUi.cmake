# Editor and in-game UI: Ember (external/ember, https://github.com/Tebarem/ember, zlib): HTML, CSS
# and JavaScript drawn with SDL_GPU on the engine's own device (engine/ui/ui_layer.cpp).
#
# Ember is C23 (#embed), which MSVC does not compile: it is built as its own CMake project with
# clang (on Windows LLVM's clang for the MSVC ABI, with the static C runtime the engine uses; on
# Linux and macOS the engine's C compiler, which must be GCC 15+ or Clang 19+), against the engine's
# patched SDL (FalconSDL.cmake: the same headers), and linked into falcon.exe as a static library:
# one SDL in the process, so the page draws with the engine's SDL_GPUDevice.
option(FALCON_UI "Editor and in-game UI through Ember" ON)
set(FALCON_EMBER_DIR "${CMAKE_SOURCE_DIR}/external/ember" CACHE PATH
  "Ember's source tree (the submodule; point it at another checkout to work on Ember)")

if(FALCON_UI AND NOT EXISTS "${FALCON_EMBER_DIR}/CMakeLists.txt")
  message(WARNING "FALCON_UI: no Ember in ${FALCON_EMBER_DIR} (git submodule update --init "
    "external/ember); building without UI")
  set(FALCON_UI OFF CACHE BOOL "" FORCE)
endif()

# Ember's own submodules that its library needs (SDL comes from the engine; its tests' and
# networking's are not built): fetched here, since the engine's checkout is not recursive.
if(FALCON_UI)
  set(fx_ember_deps third_party/SDL_image third_party/SheenBidi third_party/kb)
  set(fx_ember_missing "")
  foreach(d ${fx_ember_deps})
    file(GLOB fx_ember_dep_files "${FALCON_EMBER_DIR}/${d}/*")
    if(NOT fx_ember_dep_files)  # (an uninitialized submodule is an empty folder)
      list(APPEND fx_ember_missing "${d}")
    endif()
  endforeach()
  if(fx_ember_missing)
    message(STATUS "FALCON_UI: fetching Ember's ${fx_ember_missing}")
    execute_process(COMMAND git -C "${FALCON_EMBER_DIR}" submodule update --init --depth 1 ${fx_ember_missing}
      RESULT_VARIABLE fx_ember_fetch)
    if(NOT fx_ember_fetch EQUAL 0)
      message(FATAL_ERROR "FALCON_UI: could not fetch Ember's ${fx_ember_missing} (git -C external/ember "
        "submodule update --init ${fx_ember_missing}), or configure with -DFALCON_UI=OFF")
    endif()
  endif()
endif()

if(FALCON_UI)
  include(ExternalProject)
  set(fx_ember_args
    -DCMAKE_BUILD_TYPE=Release
    -DEMBER_TESTS=OFF
    -DEMBER_EXAMPLES=OFF
    -DEMBER_NET=OFF
    "-DEMBER_SDL_DIR=${FX_SDL_ROOT}/SDL"
    "-DEMBER_SLANGC=${FALCON_SLANG_ROOT}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
  if(MSVC)
    # LLVM's clang (the GNU-style driver) targets the MSVC ABI by default on Windows.
    find_program(FX_EMBER_CC clang
      HINTS "$ENV{ProgramFiles}/LLVM/bin" "$ENV{VCINSTALLDIR}Tools/Llvm/x64/bin" "$ENV{LLVM_PATH}/bin")
    get_filename_component(fx_llvm_bin "${FX_EMBER_CC}" DIRECTORY)
    find_program(FX_EMBER_AR llvm-ar HINTS "${fx_llvm_bin}")
    if(NOT FX_EMBER_CC OR NOT FX_EMBER_AR)
      message(FATAL_ERROR "FALCON_UI: Ember needs LLVM's clang and llvm-ar (C23); install LLVM 19+ "
        "(winget install LLVM.LLVM) or configure with -DFALCON_UI=OFF")
    endif()
    list(APPEND fx_ember_args
      "-DCMAKE_C_COMPILER=${FX_EMBER_CC}"
      "-DCMAKE_AR=${FX_EMBER_AR}"
      "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>"
      # (CodeView debug info: falcon.pdb names Ember's functions in freeze reports)
      "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -g -gcodeview")
  else()
    # A C23 compiler with #embed: gcc 15+ or clang 19+ (Ubuntu 24.04's gcc-15 / clang-19 packages,
    # Homebrew's llvm on macOS: Apple's clang has no #embed), else the engine's C compiler.
    if(APPLE)
      find_program(FX_EMBER_CC clang HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
    endif()
    find_program(FX_EMBER_CC NAMES gcc-15 gcc-16 clang-21 clang-20 clang-19)
    if(NOT FX_EMBER_CC)
      set(FX_EMBER_CC "${CMAKE_C_COMPILER}")
    endif()
    list(APPEND fx_ember_args
      "-DCMAKE_C_COMPILER=${FX_EMBER_CC}"
      "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -g")
    if(APPLE)
      list(APPEND fx_ember_args "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif()
  endif()
  if(CMAKE_C_COMPILER_LAUNCHER)
    list(APPEND fx_ember_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
  endif()

  set(fx_ember_bin "${CMAKE_BINARY_DIR}/_deps/ember-build")
  set(fx_ember_lib "${fx_ember_bin}/${CMAKE_STATIC_LIBRARY_PREFIX}ember${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(fx_ember_image "${fx_ember_bin}/third_party/SDL_image/${CMAKE_STATIC_LIBRARY_PREFIX}SDL3_image${CMAKE_STATIC_LIBRARY_SUFFIX}")
  ExternalProject_Add(falcon_ember_build
    SOURCE_DIR "${FALCON_EMBER_DIR}"
    BINARY_DIR "${fx_ember_bin}"
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS ${fx_ember_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build "${fx_ember_bin}" --target ember SDL3_image-static
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON  # (Ninja's own dependency check: a no-op when Ember did not change)
    BUILD_BYPRODUCTS "${fx_ember_lib}" "${fx_ember_image}"
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD ON)

  add_library(falcon_ember STATIC IMPORTED GLOBAL)
  set_target_properties(falcon_ember PROPERTIES IMPORTED_LOCATION "${fx_ember_lib}")
  target_include_directories(falcon_ember INTERFACE "${FALCON_EMBER_DIR}/include")
  target_link_libraries(falcon_ember INTERFACE "${fx_ember_image}" SDL3::SDL3)
  add_dependencies(falcon_ember falcon_ember_build)

  # Falcon's own page bundles (ui/editor, ui/hud, ui/launcher) next to the game as bin/falcon-ui/.
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
