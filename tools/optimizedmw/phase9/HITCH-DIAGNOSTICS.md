# Phase 9 diagnostic detail revision 2

This expansion follows the complete September 30 captures from engine `49896ef627`. Their large intervals included Lua worker work outside named callbacks, animation-controller setup outside resource probes, image lookup before decoding, and a cull envelope that combined SceneView acquisition and traversal. It adds observations; it does not promote a performance candidate. The original reference remains mode 1. Modes 3, 4, 12 and 13 arm the detail probes. Mode 3 is the first diagnostic control to test.

| Earlier uncertainty | New evidence |
| --- | --- |
| Lua worker residual | Profiler boundary, player registry, object lists, autostart, prune, event finalization, timers, events, callbacks, local/global update, tracker unload and GC step spans |
| Script initialization | Container, script path, ensure-loaded, onInit/onLoad and existing detailed script chunk/environment/package/module/body phase writer |
| Main-thread delayed Lua actions | Each action's name, nested resource operations and teleport action, rather than only the slowest action |
| Image lookup outside decode | Cache lock acquisition versus locked lookup/insertion, VFS resolve/open, physical open/read, archive extraction/decompression, reader discovery and postprocessing |
| Controller setup | Additional-source discovery, keyframe cache/parse/build, node map, clone/bind, deferred batch/chunks/join/publication, source assignment and blend rules |
| Cull envelope | Exact OSG 3.6.5 SceneView acquisition, update, traversal, statistics and queue publication boundaries |
| World update residual | Weather, navigator update, player, scene, rendering update, sound, spell preload and required navigator wait |
| Selected driver calls | Existing CPU call envelopes now carry native Windows thread IDs; explicit terrain/PostFX mipmap owners can supply texture object and GL name without a query |
| Windows scheduling and driver queue waits | Optional ETW capture with context-switch/ready-thread stacks, CPU samples, disk/file I/O, hard faults, DPC/interrupt activity and DxgKrnl GPU events |

## Engine capture

Run `START-OptimizedMW-Test.bat`, choose **3**, use the same outdoor save, heavy fixed view and route, and quit normally. The ZIP contains the raw timeline, writer/trace loss records, full Lua update and controller/script setup writers, and frame pacing analysis. `HITCH-PHASE-TOTALS.csv` summarizes retained phase exposure; `HITCH-REMAINING-SCOPES.csv` shows the largest residual intervals. The raw timeline retains parent IDs, native Windows thread IDs, logical asset paths and timestamps for detailed reconstruction.

The detail timeline has a fixed 262,144-row arena, about 160 MiB. The resource catalog remains 131,072 entries. Rows below 0.25 ms can be filtered, but their time is still included in the parent's child total. Arena overflow, writer drops, missing completion, incoherent parents or missing requested channels invalidate the affected capture. The report also accepts the earlier 131,072-row timeline format so historical captures can still be read.

Inclusive nested scopes must not be added together. Phase totals cover retained spans; different threads overlap. `unattributed_ms` means time outside instrumented direct children, which can include known leaf operations and instrumentation overhead. Windows GetThreadTimes is coarse, and short CPU deltas can exceed the corresponding wall interval. These values do not establish precise scheduler wait time.

## Optional Windows system trace

1. Right-click `START-OptimizedMW-SystemTrace.bat` and choose **Run as administrator**. This starts only the recorder. Keep that window open.
2. Start the game using the ordinary `START-OptimizedMW-Test.bat` in a separate, normal window and choose **3**. Do not run the game from the elevated recorder.
3. Reproduce the hitch in a short run, ideally 30-60 seconds after loading. Quit normally and immediately press Enter in the recorder window. For a specific hitch, stop as soon as practical after it.
4. Share the normal mode 3 ZIP and `SystemTrace/<timestamp>/OptimizedMW-SystemTrace.etl` with `SYSTEM-TRACE-STATE.json` from the same run.

The custom WPR profile uses circular memory buffers totaling 320 MiB. It does not stream an ever-growing recording to disk while playing. Saving writes one compressed ETL plus metadata; merge overhead and final ETL size are not limited to the buffer size. Circular capture can overwrite early events, so retained time ranges and ETL lost-event counts must be inspected before drawing conclusions. Recorder success is not a completeness result.

The helper uses a unique WPR instance and only stops/cancels that instance. A failed start does not trigger cancellation of another recording. It records executable/profile hashes, WPR start/stop output and recent mode 3 capture identities. On interruption after a successful start, it attempts to cancel its own session in `finally`. If the recorder is forcibly killed, its session can remain active; use the recorded instance name for recovery. It does not change game settings or launch the game with elevated privileges.

`p9-hitch-spans.csv.clock-map.txt` supplies process ID, native thread ID convention, QPC frequency, paired steady-clock/QPC brackets at start/end, UTC anchors and normal completion. Align a span's steady timestamp with QPC using these brackets and check drift/uncertainty; do not assume that epoch milliseconds or independent frame IDs imply exact alignment. Selected GL rows use the same steady clock and include their native thread ID.

In Windows Performance Analyzer, narrow the ETL to the matching process/thread and calibrated hitch interval. CPU Usage (Precise) separates running, ready and blocked intervals and identifies the thread that made a blocked thread runnable. Sampled stacks, disk/file I/O, hard faults and DPC/interrupt tables test alternative causes. GPU events can identify queue activity and overlapping execution where the driver emits them. Match exact `openmw.exe` and `openmw.pdb`; matching engine symbols remain in the local build directory recorded in `Validation/BUILD-AND-TEST-STATE.json`. WPA is not installed by the game package.

NVIDIA private driver implementation and missing private symbols remain partly opaque. A long GL call alone does not prove shader compilation, GPU execution, residency eviction, or an OS scheduling delay. ETW scheduling and GPU events can narrow those alternatives, but only the captured evidence can justify a diagnosis. WPR profile validation is automated; actual elevated recording and runtime hitch diagnosis require a user capture.

Sources: [Microsoft WPR start/stop reference](https://devblogs.microsoft.com/performance-diagnostics/wpr-start-and-stop-commands/), [Microsoft WPR/WPA process and thread troubleshooting](https://learn.microsoft.com/en-us/troubleshoot/windows-server/support-tools/support-tools-xperf-wpa-wpr), [OSG 3.6.5 Renderer implementation](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgViewer/Renderer.cpp).
