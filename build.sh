#!/usr/bin/env bash
# Configures and builds Falcon on Linux or macOS (build.ps1 is Windows').
#   ./build.sh [--run] [-- args for falcon]
# Needs CMake 3.25+, Ninja, a C++20 compiler (gcc 13+ / clang 17+ / Xcode 16+), the .NET 10 SDK and
# Slang (fetched into ~/slang-<version>-<os>-<arch> when missing: scripts/fetch.cmake), and CEF
# for the editor (into external/cef-lite/vendor/cef; FALCON_UI=OFF builds without it).
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
  Linux) preset=linux ;;
  Darwin) preset=macos ;;
  *) echo "build.sh: Linux or macOS (on Windows: build.ps1)" >&2; exit 1 ;;
esac
run=0
if [ "${1:-}" = "--run" ]; then run=1; shift; fi
if [ "${1:-}" = "--" ]; then shift; fi

cd "$root"
cmake -DDEP=slang -P scripts/fetch.cmake
cmake -DDEP=cef -P scripts/fetch.cmake
cmake --preset "$preset"
cmake --build --preset "$preset"
if [ "$run" = 1 ]; then
  exec "$root/build/$preset/bin/falcon" "$@"
fi
