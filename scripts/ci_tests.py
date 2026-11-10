#!/usr/bin/env python3
"""Runs the tests a machine can run without a GPU (GitHub's hosted runners), or lists them.

Every test comes from `ctest --show-only=json-v1` and is sorted by what it runs:
  gpu  the command opens a window: falcon --hidden, --preview, --project or --editor
  cpu  everything else: falcon cook / --server / vfx-test / dmath-hash, the level, net and BVH
       test tools
A comparison (falcon_imgdiff, falcon_wavdiff, a cmake script) is gpu when a test it DEPENDS on, or
whose fixture it requires, is gpu: ctest -R would run it without its inputs otherwise.

  python scripts/ci_tests.py --build build/profile                  # run the cpu tests
  python scripts/ci_tests.py --build build/profile --kind gpu --list
  python scripts/ci_tests.py --build build/profile -- --timeout 600 # extra ctest arguments
  python scripts/ci_tests.py --build build/linux --kind gpu --only '^render_naive'  # a few, with
                                                     # the tests they DEPENDS on (fixtures: ctest)
"""

import argparse
import json
import os
import re
import subprocess
import sys

GPU_FIRST_ARGS = {"--hidden", "--preview", "--project"}


def as_list(value):
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


def classify(tests):
    info = {}
    for test in tests:
        command = test.get("command") or []
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        exe = os.path.splitext(os.path.basename(command[0]))[0].lower() if command else ""
        info[test["name"]] = {
            "exe": exe,
            "args": command[1:],
            "requires": as_list(props.get("FIXTURES_REQUIRED")),
            "setup": as_list(props.get("FIXTURES_SETUP")),
            "depends": as_list(props.get("DEPENDS")),
        }

    kind = {}
    for name, t in info.items():
        args = t["args"]
        opens_window = t["exe"] == "falcon" and ((args and args[0] in GPU_FIRST_ARGS) or "--editor" in args)
        kind[name] = "gpu" if opens_window else "cpu"

    setups = {}
    for name, t in info.items():
        for fixture in t["setup"]:
            setups.setdefault(fixture, []).append(name)

    changed = True
    while changed:
        changed = False
        for name, t in info.items():
            if kind[name] == "gpu":
                continue
            upstream = [s for f in t["requires"] for s in setups.get(f, [])]
            upstream += [d for d in t["depends"] if d in info]
            if any(kind[u] == "gpu" for u in upstream):
                kind[name] = "gpu"
                changed = True
    return kind, info


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, help="the CMake build folder (build/profile)")
    parser.add_argument("--kind", choices=("cpu", "gpu"), default="cpu")
    parser.add_argument("--exclude", help="regex of test names to leave out")
    parser.add_argument("--only", help="regex of the test names to run (and what they DEPENDS on)")
    parser.add_argument("--list", action="store_true", help="print the names and stop")
    parser.add_argument("ctest_args", nargs="*", help="after --: passed to ctest")
    args = parser.parse_args()

    shown = subprocess.run(["ctest", "--show-only=json-v1"], cwd=args.build, check=True,
                           capture_output=True, text=True)
    kind, info = classify(json.loads(shown.stdout)["tests"])
    names = sorted(n for n, k in kind.items() if k == args.kind)
    if args.only:
        chosen = {n for n in names if re.search(args.only, n)}
        todo = list(chosen)
        while todo:  # (fixture setups ctest adds by itself; DEPENDS it doesn't)
            for d in info[todo.pop()]["depends"]:
                if d in info and d not in chosen:
                    chosen.add(d)
                    todo.append(d)
        names = sorted(chosen)
    if args.exclude:
        names = [n for n in names if not re.search(args.exclude, n)]

    if args.list:
        print("\n".join(names))
        print(f"{len(names)} {args.kind} tests of {len(kind)}", file=sys.stderr)
        return 0
    if not names:
        print(f"no {args.kind} tests", file=sys.stderr)
        return 0

    regex = "^(" + "|".join(re.escape(n) for n in names) + ")$"
    print(f"running {len(names)} {args.kind} tests of {len(kind)}", flush=True)
    return subprocess.run(["ctest", "-R", regex, *args.ctest_args], cwd=args.build).returncode


if __name__ == "__main__":
    sys.exit(main())
