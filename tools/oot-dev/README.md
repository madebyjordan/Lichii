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
