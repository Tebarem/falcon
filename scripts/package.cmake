# cmake -DBUILD=<build folder> -DOUT=<folder> -DNAME=<package name> [-DVERSION=v1.2.3] -P scripts/package.cmake
# Packages a build's bin/ for this OS, as the release pipeline ships it:
#   Windows  <NAME>.zip     bin/ (falcon.pdb kept: freeze reports name functions with it)
#   Linux    <NAME>.tar.gz  bin/ (falcon, its libraries and data; executables stay executable)
#   macOS    <NAME>.zip     Falcon.app: bin/'s code (Mach-O files, the helper apps, the CEF
#                           framework) in Contents/MacOS, everything else in Contents/Resources,
#                           each tree linking to the other's files so bin/'s whole layout is in
#                           both (executableDir() is Contents/MacOS, SDL_GetBasePath()
#                           Contents/Resources); an Info.plist; signed ad hoc so it opens
#                           (Gatekeeper still asks once: it isn't notarized)
# Left out everywhere: linker leftovers (.ilk, .lib, .exp), the NativeAOT symbol files (.dbg) and
# CMake's own files.

cmake_minimum_required(VERSION 3.25)
foreach(var BUILD OUT NAME)
  if(NOT ${var})
    message(FATAL_ERROR "package.cmake: -D${var}=... is required")
  endif()
endforeach()
get_filename_component(BUILD "${BUILD}" ABSOLUTE)
get_filename_component(OUT "${OUT}" ABSOLUTE)
set(bin "${BUILD}/bin")
if(NOT IS_DIRECTORY "${bin}")
  message(FATAL_ERROR "package.cmake: no ${bin}")
endif()

set(stage "${OUT}/${NAME}")
file(REMOVE_RECURSE "${stage}")
if(CMAKE_HOST_APPLE)
  set(app "${stage}/Falcon.app")
  set(code_dir "${app}/Contents/MacOS")
  set(data_dir "${app}/Contents/Resources")
  set(dest "${stage}/bin")  # (split into the two below)
else()
  set(dest "${stage}")
endif()
file(MAKE_DIRECTORY "${dest}")
file(COPY "${bin}/" DESTINATION "${dest}" USE_SOURCE_PERMISSIONS
  PATTERN "*.ilk" EXCLUDE PATTERN "*.lib" EXCLUDE PATTERN "*.exp" EXCLUDE PATTERN "*.dbg" EXCLUDE
  PATTERN "FreezeReports" EXCLUDE PATTERN "CMakeFiles" EXCLUDE)

if(CMAKE_HOST_APPLE)
  # codesign takes every file in Contents/MacOS for code that must be signed (a .slang there:
  # "code object is not signed at all"); Contents/Resources is the bundle's data. The code:
  # Mach-O files (symlinks by what they point at) and whole bundles (the outermost .app or
  # .framework in a path).
  file(GLOB_RECURSE all LIST_DIRECTORIES false RELATIVE "${dest}" "${dest}/*")
  set(code)
  foreach(rel IN LISTS all)
    set(unit "")
    foreach(ext .app/ .framework/)
      string(FIND "${rel}" "${ext}" at)
      if(at GREATER -1)
        string(LENGTH "${ext}" n)
        math(EXPR n "${at} + ${n} - 1")
        string(LENGTH "${unit}" unit_n)
        if(NOT unit OR n LESS unit_n)
          string(SUBSTRING "${rel}" 0 ${n} unit)
        endif()
      endif()
    endforeach()
    if(NOT unit)
      execute_process(COMMAND file -bL "${dest}/${rel}" OUTPUT_VARIABLE kind OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(kind MATCHES "Mach-O")
        set(unit "${rel}")
      endif()
    endif()
    if(unit)
      list(APPEND code "${unit}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES code)

  # place(<rel> <depth>): moves bin/<rel>'s entries to code_dir or data_dir and links each from
  # the other tree (relative links: up to Contents/, then into MacOS/ or Resources/). A folder
  # with no code in it moves (and is linked) whole; one with code is placed entry by entry.
  function(place rel depth)
    set(up "")
    foreach(i RANGE ${depth})
      string(APPEND up "../")
    endforeach()
    if(rel STREQUAL "")
      set(src "${dest}")
      set(prefix "")
    else()
      set(src "${dest}/${rel}")
      set(prefix "${rel}/")
    endif()
    file(MAKE_DIRECTORY "${code_dir}/${rel}" "${data_dir}/${rel}")
    file(GLOB entries LIST_DIRECTORIES true RELATIVE "${src}" "${src}/*")
    foreach(e IN LISTS entries)
      set(path "${prefix}${e}")
      set(has_code FALSE)
      if(IS_DIRECTORY "${src}/${e}" AND NOT IS_SYMLINK "${src}/${e}")
        foreach(c IN LISTS code)
          string(FIND "${c}" "${path}/" at)
          if(at EQUAL 0)
            set(has_code TRUE)
            break()
          endif()
        endforeach()
      endif()
      if(path IN_LIST code)
        file(RENAME "${src}/${e}" "${code_dir}/${path}")
        execute_process(COMMAND ln -s "${up}MacOS/${path}" "${data_dir}/${path}" COMMAND_ERROR_IS_FATAL ANY)
      elseif(has_code)
        math(EXPR deeper "${depth} + 1")
        place("${path}" ${deeper})
      else()
        file(RENAME "${src}/${e}" "${data_dir}/${path}")
        execute_process(COMMAND ln -s "${up}Resources/${path}" "${code_dir}/${path}" COMMAND_ERROR_IS_FATAL ANY)
      endif()
    endforeach()
  endfunction()
  place("" 0)
  file(GLOB_RECURSE left LIST_DIRECTORIES false "${dest}/*")
  if(left)
    message(FATAL_ERROR "package.cmake: not placed in Falcon.app: ${left}")
  endif()
  file(REMOVE_RECURSE "${dest}")
  foreach(rel IN LISTS all)
    if(NOT EXISTS "${code_dir}/${rel}" OR NOT EXISTS "${data_dir}/${rel}")
      message(FATAL_ERROR "package.cmake: ${rel} is not reachable from both Contents/MacOS and Contents/Resources")
    endif()
  endforeach()
  list(LENGTH code code_n)
  list(LENGTH all all_n)
  message(STATUS "Falcon.app: ${code_n} pieces of code in Contents/MacOS, the rest of ${all_n} files in Contents/Resources")

  set(version "${VERSION}")
  if(NOT version)
    set(version "0.0.0")
  endif()
  string(REGEX REPLACE "^v" "" version "${version}")
  string(REGEX REPLACE "[^0-9.].*$" "" short_version "${version}")
  file(WRITE "${app}/Contents/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleName</key><string>Falcon</string>
  <key>CFBundleDisplayName</key><string>Falcon Engine</string>
  <key>CFBundleIdentifier</key><string>gg.falconnetwork.falcon</string>
  <key>CFBundleExecutable</key><string>falcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>CFBundleShortVersionString</key><string>${short_version}</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSMicrophoneUsageDescription</key><string>Voice chat in games made with Falcon.</string>
</dict>
</plist>
")
  # Inside out (codesign --deep is deprecated): the loose libraries and executables, then the
  # nested bundles (the CEF framework, the Falcon Helper apps), then the app. A
  # bundle's own executable is signed with its bundle.
  file(GLOB_RECURSE files LIST_DIRECTORIES false "${code_dir}/*")
  set(bundles)
  foreach(f IN LISTS files)
    if(IS_SYMLINK "${f}")
      continue()
    endif()
    execute_process(COMMAND file -b "${f}" OUTPUT_VARIABLE kind OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT kind MATCHES "Mach-O" OR f STREQUAL "${code_dir}/falcon")
      continue()
    endif()
    get_filename_component(name "${f}" NAME)
    if(f MATCHES "/([^/]+)\\.app/Contents/MacOS/([^/]+)$" AND CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
      string(REGEX REPLACE "/Contents/MacOS/[^/]+$" "" bundle "${f}")
      list(APPEND bundles "${bundle}")
    elseif(f MATCHES "/([^/]+)\\.framework/(Versions/[^/]+/)?([^/]+)$" AND CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_3)
      string(REGEX REPLACE "(\\.framework)/.*$" "\\1" bundle "${f}")
      list(APPEND bundles "${bundle}")
    else()
      execute_process(COMMAND codesign --force --sign - "${f}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
  endforeach()
  # (none holds another: their order does not matter)
  list(REMOVE_DUPLICATES bundles)
  foreach(bundle IN LISTS bundles)
    execute_process(COMMAND codesign --force --sign - "${bundle}" COMMAND_ERROR_IS_FATAL ANY)
  endforeach()
  execute_process(COMMAND codesign --force --sign - "${app}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND codesign --verify "${app}" COMMAND_ERROR_IS_FATAL ANY)
  # (--strict also judges where nested code sits: the helpers live beside falcon in
  # Contents/MacOS, not Contents/Frameworks; say what it thinks without failing)
  execute_process(COMMAND codesign --verify --strict --verbose=1 "${app}" RESULT_VARIABLE strict_rc)
  if(NOT strict_rc EQUAL 0)
    message(WARNING "package.cmake: codesign --verify --strict is unhappy with ${app} (ad-hoc signed; it opens all the same)")
  endif()
  file(REMOVE "${OUT}/${NAME}.zip")
  # (ditto keeps the bundle's symlinks and signatures as Finder's Compress does)
  execute_process(COMMAND ditto -c -k --keepParent "${app}" "${OUT}/${NAME}.zip" COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "Packaged ${OUT}/${NAME}.zip")
elseif(CMAKE_HOST_WIN32)
  file(REMOVE "${OUT}/${NAME}.zip")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${NAME}.zip" --format=zip "${NAME}" WORKING_DIRECTORY "${OUT}"
    COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "Packaged ${OUT}/${NAME}.zip")
else()
  file(REMOVE "${OUT}/${NAME}.tar.gz")
  execute_process(COMMAND tar -czf "${OUT}/${NAME}.tar.gz" -C "${OUT}" "${NAME}" COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "Packaged ${OUT}/${NAME}.tar.gz")
endif()
file(REMOVE_RECURSE "${stage}")
