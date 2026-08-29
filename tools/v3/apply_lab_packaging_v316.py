from pathlib import Path
import subprocess

# Apply the complete validated V3.15 stack first, then V3.16 general-play hitch work.
v315 = Path(__file__).with_name("apply_lab_packaging_v315.py")
exec(
    compile(v315.read_text(encoding="utf-8"), str(v315), "exec"),
    {"__file__": str(v315), "__name__": "__main__"},
)

v316 = Path(__file__).with_name("apply_v316_general_play_hitch.py")
exec(
    compile(v316.read_text(encoding="utf-8"), str(v316), "exec"),
    {"__file__": str(v316), "__name__": "__main__"},
)

# Buffered ambient SFX are a different path from streamed voice/music. Keep
# already-decoded OpenAL Soft buffers resident much more aggressively in the
# balanced/aggressive V3.16 modes to reduce repeat decode/I/O churn.
v316_sfx = Path(__file__).with_name("apply_v316_sfx_retention.py")
exec(
    compile(v316_sfx.read_text(encoding="utf-8"), str(v316_sfx), "exec"),
    {"__file__": str(v316_sfx), "__name__": "__main__"},
)

# Aggressive Mode89 also predecodes likely ESM-backed SFX on one idle-priority
# worker into a bounded PCM reservoir. No OpenAL calls happen on the worker;
# requested sounds that are not ready fall through to the original sync path.
v316_sfx_predecode = Path(__file__).with_name("apply_v316_sfx_predecode.py")
exec(
    compile(v316_sfx_predecode.read_text(encoding="utf-8"), str(v316_sfx_predecode), "exec"),
    {"__file__": str(v316_sfx_predecode), "__name__": "__main__"},
)
v316_sfx_predecode_fix = Path(__file__).with_name("apply_v316_sfx_predecode_compilefix.py")
exec(
    compile(v316_sfx_predecode_fix.read_text(encoding="utf-8"), str(v316_sfx_predecode_fix), "exec"),
    {"__file__": str(v316_sfx_predecode_fix), "__name__": "__main__"},
)

# Periodic ResourceSystem maintenance already runs off the gameplay thread, but
# V3.15 evidence suggests its shared-queue CPU/memory contention can still show
# up as a regular small frametime pulse. Balanced/aggressive modes move it to a
# dedicated idle-priority worker without changing expiry semantics or cadence.
v316_resource = Path(__file__).with_name("apply_v316_idle_resource_sweep.py")
exec(
    compile(v316_resource.read_text(encoding="utf-8"), str(v316_resource), "exec"),
    {"__file__": str(v316_resource), "__name__": "__main__"},
)

# V3.15 lab evidence showed synchronous CSV output can contaminate the same
# frametime tails we are trying to measure. Move diagnostic file I/O off producer
# threads before taking the generated-source snapshot.
v316_async = Path(__file__).with_name("apply_v316_async_diagnostics.py")
exec(
    compile(v316_async.read_text(encoding="utf-8"), str(v316_async), "exec"),
    {"__file__": str(v316_async), "__name__": "__main__"},
)

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.16 general-play hitch suppression
====================================

Primary objective
-----------------
V3.16 freezes the promoted V3.15 Mode84 renderer/paging architecture and attacks
ordinary gameplay frametime spikes that occur while standing still, traversing,
or approaching actors. V3.15 logs showed repeated sound/script stalls plus
actor-local Lua bursts, smaller periodic resource cleanup pulses, and a separate
render-traversal synchronization lane.

Streamed sound head cache
-------------------------
V3.16 backports official OpenMW upstream commit
9ec49cfb4709cbfd8f14e97f5b9a558b71b8184f (sound-file head cache). Streamed
music/voice initialization can reuse the exact prefix/suffix bytes FFmpeg needed
on the first open, moving subsequent initialization reads to RAM. The feature is
configured through [Sound] 'head cache size'. V3.16 keeps the engine default at
0 MB so the control remains exact; experiment modes explicitly select 64/128 MB.
The upstream per-file safety ceiling remains 256 KiB.

Buffered SFX retention
----------------------
OpenMW Lua ambient playSound/playSoundFile calls use SoundBufferPool and
OpenALOutput::loadSound: the complete file is synchronously decoded and uploaded
the first time a nonresident SFX is requested. The stock decoded-buffer cache is
only 56/64 MB, which can churn in a large audio/mod setup and force later sounds
to repeat this work. Mode88 raises the decoded SFX cache to 256/384 MB and Mode89
to 512/768 MB. These are system/OpenAL Soft memory budgets, not texture/geometry
VRAM budgets. Mode87 deliberately leaves the user's existing buffer-cache limits
untouched so it remains a clean streamed-head-cache isolation run.

Aggressive first-use SFX predecode
----------------------------------
Mode89 additionally enables a 384 MB PCM predecode reservoir with one
idle-priority worker. Once a game is active, the main thread enumerates known ESM
sound resource names and queues them. The worker performs only VFS/FFmpeg decode;
it never calls OpenAL. OpenAL buffer creation/upload remains on the gameplay
thread. If a requested sound is already predecoded, the synchronous storage and
FFmpeg decode are skipped. If it is not ready, the exact original synchronous
load path runs immediately, so playback semantics and correctness are preserved.
Individual speculative decoded entries are capped at 16 MB, the total ready
reservoir is bounded, and the worker waits for reservoir space rather than growing
memory without limit. Direct arbitrary playSoundFile paths that are not represented
by ESM sound records still use the original first-load path unless already known.

Idle-priority resource maintenance
----------------------------------
V3.7 already made ResourceSystem cache sweeps pressure-aware and moved deletion
work off the main thread, but those jobs still ran at the front of the same normal-
priority WorkQueue used by paging/preloading. Modes88/89 create one dedicated
resource-maintenance WorkQueue and permanently lower only that worker to idle
priority before ResourceSystem::updateCache. The V3.7 sweep cadence, cache expiry
rules, adapter-pressure policy, and emergency behavior are unchanged. Paging and
other gameplay-critical preload workers never inherit the lowered thread priority.

Asynchronous lab diagnostics
----------------------------
V3.16 removes synchronous CSV writes and periodic flushes from diagnostic
producer/gameplay threads. Each enabled CsvWriter queues complete rows into a
bounded in-memory queue and a dedicated writer thread performs disk output.
The queue is capped at 4096 rows; if diagnostic storage cannot keep up, V3.16
drops diagnostic rows instead of blocking gameplay. The writer records the drop
count at shutdown. This transport applies to all V3.16 modes so A/B measurement
is fair and the lab itself is less likely to manufacture periodic frametime
spikes. No writer thread exists for disabled diagnostic streams.

Runtime matrix
--------------
86 = exact V3.15 Mode84 gameplay settings control, sound head cache 0 MB
87 = Mode86 + 64 MB streamed-audio head cache; existing decoded-SFX cache unchanged
88 = balanced V3.16 hitch candidate: Mode84 + audio head 64 MB + decoded SFX
     cache 256/384 MB + dedicated idle resource maintenance + common async diagnostics
89 = aggressive V3.16 hitch candidate: Mode84 + audio head 128 MB + decoded SFX
     cache 512/768 MB + 384 MB/1-worker first-use SFX predecode + dedicated idle
     resource maintenance + common async diagnostics

Development policy
------------------
Do not weaken Lua/script semantics to hide expensive mods. Do not defer required
scene construction or V3.13 strong-wins cache installation. OpenAL operations stay
on their existing thread/context. Resource expiry semantics remain unchanged.
Further render-thread changes must identify an actual synchronization point rather
than applying generic deferral. Diagnostic I/O must not become a gameplay-thread
hitch source.
''')

ROOT = Path(__file__).resolve().parents[2]
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = (ROOT / "V3-LAB-README.txt").read_text(encoding="utf-8")
for marker in (
    "head cache size",
    "mHeadCacheSize",
    "class HeadCache",
    "getStreamDecoder",
    "v316-mode84-control",
    "v316-audio64",
    "v316-balanced-hitch",
    "v316-aggressive-hitch",
    "V316BufferCacheMin",
    "V316BufferCacheMax",
    "SfxPredecodeCache",
    "sfx predecode cache size",
    "V316SfxPredecodeCacheSize",
    "getResourceNamesForPredecode",
    "mV316IdleResourceSweep",
    "v3.16 idle resource sweep",
    "mV316ResourceSweepQueue",
    "sMaxQueuedLines",
    "v3_async_diagnostics_dropped_lines",
):
    if marker not in patch_text and marker not in launcher_text:
        raise RuntimeError(f"V3.16 generated source snapshot missing marker: {marker}")

if "V3.16 general-play hitch suppression" not in readme_text:
    raise RuntimeError("V3.16 README identity marker missing")
if "Asynchronous lab diagnostics" not in readme_text:
    raise RuntimeError("V3.16 asynchronous diagnostics README marker missing")
if "Buffered SFX retention" not in readme_text:
    raise RuntimeError("V3.16 buffered SFX retention README marker missing")
if "Aggressive first-use SFX predecode" not in readme_text:
    raise RuntimeError("V3.16 SFX predecode README marker missing")
if "Idle-priority resource maintenance" not in readme_text:
    raise RuntimeError("V3.16 idle resource maintenance README marker missing")

print("V3.16 exact generated-source snapshot refreshed after complete V3.16 layer.")
