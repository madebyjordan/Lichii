# OoT dev tools (madebyjordan fork only)

Local helpers for testing Ocarina of Time (`0400486024DEC000`) fixes on Windows. They aren't meant for upstream.
Each launcher runs `build\bin\lichii.exe` from this checkout.

| File | What it does |
|---|---|
| `run-dev.bat` | Normal test run. Turns on the pipeline build trace (`build\bin\pipeline-build-trace.txt`, the previous one kept as `.old.txt`) and `EDEN_NX2_ASYNC_DISCARD=1`. |
| `run-budget-test.bat` | Same, with the Switch 2 pipeline budget forced to 1500 (`EDEN_NX2_TEST_PIPELINE_BUDGET`) to exercise eviction. |
| `ab/prepare.bat` | Restores the pipeline lists from `ab/snapshot\` and clears AMD's Vulkan shader cache, so A/B runs start from the same state. |
| `ab/run-a.bat`, `ab/run-b.bat` | A/B boots writing `ab/trace-A.txt` / `ab/trace-B.txt`. B sets `EDEN_NX2_NOOPT_SHADERS`; edit the hash list for a new test. |

Async shaders must be on for OoT (per-game setting) for `EDEN_NX2_ASYNC_DISCARD` to do anything.
Create the A/B snapshot first: copy `%APPDATA%\eden\cache\shader\0400486024dec000\*.bin` into `ab\snapshot\`. That folder is git-ignored by `ab/.gitignore`.

Test-only code these rely on is in the `LOCAL TEST ONLY` commit on this branch.

## Findings so far (2026-11-06, RX 6700 XT, AMD driver 2.0.353)

- zortos293/Lichii#313: the eviction budget is verified in normal play and with a forced budget. Evicting recently used pipelines lost the device, hence the 9000-frame floor.
- `EDEN_NX2_ASYNC_DISCARD=1` with async shaders: time spent with draws held for unbuilt pipelines falls from ~86 s to under 0.5 s per session.
- Building tessellation shader `da3613e0fed1f0be` unoptimized didn't help: its pipelines took −15%, total driver time was unchanged (+4%), and fps was the same.
- The remaining ~2 s freezes aren't GPU-thread pipeline builds. The next suspect is the `IStorage::Read` throttle (#278).

## Performance findings (2026-11-06, Castle Town / Hyrule Castle / Hyrule Field route, ~5 min per run)

These come from the `perf/run-profile.bat` frame trace, GPU profile and IPC counts.

- Average fps is ~28. The median frame is ~40 ms: the GPU is busy ~26 ms and idle ~14 ms between ~35 submissions.
- No CPU thread is saturated. The busiest guest core uses ~630 ms/s, and the emulator ~4 of 12 host threads.
- The GPU front thread waits for the back ~400–560 ms/s, and the back waits for the front ~340–380 ms/s. This mutual waiting is what leaves the GPU idle.
- The largest single GPU item is the gameplay upscaler `6bd913e1f34e83eb` (software HMMA), ~2.3 ms per run.
- Rejected:
  - the IStorage read throttle (#278 off: same fps and the same share of frames over 100 ms)
  - ASTC decoding (0 ms waiting; the "in X ms" figure is the report window, not decode time)
  - #174, HMMA unroll (upscaler 2.29 → 2.29 ms on RDNA2; reverted on this branch)
  - #345, precise split-front reads: front sync waits fell from 416 to 247 ms/s, but writer waits rose from 148 to 279 ms/s, so fps didn't change. It also needed a shadowing rename to build on MSVC.
