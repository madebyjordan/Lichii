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
- The remaining ~2 s freezes aren't GPU-thread pipeline builds, and they aren't the `IStorage::Read` throttle either (see below).

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
- Macro `0xe2c` reads query-report pages ~8,000 times a second, and only 540 of 2.08 million of those reads hit a pending result in the exact words. The page marks are sticky and page-granular.
- #243 (per-slot report tracking, fork branch `oot/perf-243`): macro DMA waits fall from 185 to 38 ms/s, but acquire and list-writer waits rise, so total front waits stay ~534 ms/s. fps 29.3 (within noise), and green foliage flashes return. Kept #345 on oot/dev because it removes the flashes.
- Conclusion: wait-site fixes move the waiting instead of removing it. The host GPU needs ~25–27 ms per frame, so this RX 6700 XT tops out around ~38 fps at best. More fps would need cheaper back-thread draw setup.
- **Win: `EDEN_NX2_PREFLUSH=1` (#259).** Guest CPU reads of GPU-written memory fall from 68.6/s to 5.3/s. Each one cost a `Scheduler::Finish` (the whole GPU queue) on the back thread. fps went 26.6–27.3 → 29.9, frames over 100 ms −20%, and there was no flicker in Castle Town or Hyrule Field with #345 present. On by default in `run-dev.bat`. #259's own flicker (Kokiri grass and rocks flashing white or yellow) looks like the stale-report flashes #345 fixes. Kokiri isn't tested here yet.
- The submit budget (`EDEN_NX2_FLUSH_DRAWS`, default 128 draws for native titles) is already right. At 4096: 26.4 fps, 1.8% of frames over 100 ms, GPU busy +6 ms. At 64: 28.8 fps, 1.35%. Only ~3 of ~43 submissions per frame come from it.
- No flicker in Kokiri Forest village either with PREFLUSH + #345.
- Sampling profile (`perf/sample_threads.py`, Kokiri Forest, 45 s):
  - The back GPU thread is ~69% busy: ~16% of that is in `Scheduler::SubmitExecution` (driver submits), plus per-draw buffer-cache binding and syncing.
  - The front GPU thread waits ~75% of the time.
  - The game's busiest cores (4 and 5) run JIT code ~72% of the time.
  - Nothing saturates: it's a latency-bound chain from the game to the front thread, the back thread and the GPU.
- Render pass ends run ~2,600/s (~95/frame). `EDEN_NX2_DEFER_WFI` (MoltenVK's WaitForIdle deferral, tried on desktop) removed the 929/s WaitForIdle breaks, but clears and render target changes ended the passes soon after anyway: total −6%, no fps gain, no visual errors. Reverted. Most breaks follow the game's own render target changes.
