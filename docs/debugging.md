# Profiler and freeze reports

## Profiler

The editor's Profiler tab (Ctrl+7; Maximize gives it the window) records every frame.

**What it records:**
- **CPU zones** on every thread: `FX_ZONE` / `FX_ZONE_CAT` (`engine/core/profiler.h`), which
  still feed Tracy when it is compiled in.
  - Each zone has a category (Rendering, Scripts, Physics, Animation, UI, Editor, Wait, Other).
  - Workers are named and get Box3D's task zones (named by Box3D) and animation zones.
- **GPU time per pass** with timestamp queries (`engine/gpu/gpu_profiler.h`, our SDL patch).
  - One timestamp before each pass. A mark lasts until the next one, so the passes partition
    the frame's GPU time.
  - Sections (Culling, Visibility, HiZ, Shadows, Ambient Occlusion, Resolve, Post, GI, UI…)
    group about 85 passes.
  - Results come back through the renderer's fenced stats readback a few frames later.
- **Counters:**
  - culling and triangles per pass;
  - shadow pages and records;
  - virtual-texture residency and streaming;
  - physics bodies and Box3D's step stages;
  - animation, UI frames and messages;
  - present mode;
  - process working set and private bytes;
  - GPU memory per module.
- **GPU memory:** every SDL_GPU buffer, texture and transfer buffer is created through
  `engine/gpu/gpu_memory.h`, which records its module, name and size.

**What the tab shows:**
- **Charts** over the last 600 frames:
  - CPU main-thread time stacked by category;
  - GPU time stacked by section;
  - one chart per counter module. Their legends toggle series.
- **Selecting a frame:** click or drag on a chart (←/→ step through frames). Selecting pauses
  the view, and Current resumes it.
- **Views of the selected frame:**
  - **Timeline:** every thread plus a GPU lane on its own clock. Wheel zooms, drag pans, and
    hovering shows total / self times.
  - **Hierarchy:** merged calls, sortable, with search, for any thread or the GPU.
  - **Counters:** each counter with min / avg / max over the history.
  - **Memory:** every GPU resource by module and size.

**How it gets to the page:**
- Zones and GPU marks are recorded in the editor, and GPU marks only while the tab is open.
- The page gets `profiler.frames` ten times a second (numbers only; names, counters and threads
  go out incrementally), and one frame's full detail on request (`profiler.frame`).

**Cost:** on Sponza at 1600×900, `--profile` costs nothing measurable (2.002 vs 2.003 ms per
frame).

**Outside the editor:** `--profile` records the same data and logs CPU categories and GPU
sections every 2 s.

## Freeze reports

When the main thread stops finishing frames for 8 seconds (`--freeze-seconds`), a watchdog thread
(`core/freeze_watch.*`) writes what the process was doing to `FreezeReports/` beside the executable
(under `%LOCALAPPDATA%\Falcon Engine\FreezeReports` when that can't be written):

- `freeze-<date>-<time>.txt`: every thread's call stack, the main thread first. Function names and
  source lines come from `falcon.pdb` beside `falcon.exe`; release builds write it too (`/DEBUG`
  with `/OPT:REF,ICF`, so the executable is unchanged). If the main thread is still stuck 12 s
  later, its stack is added again: the same place means a deadlock or a long wait, another one a
  loop. Then the last 200 log lines.
- `freeze-<date>-<time>.dmp`: a minidump (threads, stacks and the memory they point at), for Visual
  Studio or WinDbg with the pdb.

The stacks are taken by suspending each thread in turn and unwinding it from the images' unwind
data. Nothing is allocated while a thread is suspended, because it may hold the heap's lock. Unwinding
stops at code outside the loaded images (C#'s JIT code); the minidump has the rest.

When the main thread runs again, it logs a warning naming the report, which the editor's Log tab
shows. Nothing is watched while a debugger is attached, because breakpoints stop the main thread too.
A minimized window still counts as running. Test: `freeze_report` (a 3 s sleep with a 1 s limit)
and `freeze_report_contents` (the main thread's stack at `freezewatch::testFreeze`, the log, the
dump).
