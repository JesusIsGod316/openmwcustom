# CP4F Nsight startup-argument hotfix

2026-09-23, launcher revision picking-population-profile-4.
Source branch codex/cp4f-material-frame-repair, base efa4f3694fad904b3546e7c846c756403950f0d2.
Local uncommitted helper-only repair; no engine rebuild, commit, push, or CI run.

## Failure and cause

The user's capture 20260923-165735-gameplay-83072 failed before loading configuration.
OpenMW reported the required argument for --script-run was missing.
The original Python command list contained --script-run followed by an empty string.
An actual Nsight Systems 2026.5.1 no-tracing child-process test reproduced the loss:
the child received only ["--script-run"]. The previous mocked argument-list test
did not exercise this intermediate Windows process launch.

## Repair

For opt-in Nsight runs, create a zero-byte no-op-startup.txt in the private capture
directory and pass its nonempty absolute path as --script-run's value. This keeps
the explicit override of inherited startup scripts. Console::executeFile reads no
commands from an empty file. Do not simply remove the override or supply an empty
load-savegame option. Normal direct launches retain their existing empty override.
The profiler now rejects any empty target argument before starting a tool.
Run manifests record both helper hashes and the no-op file's hash and size.

## Validation

- Gameplay diagnostic tests: 24 passed.
- Diagnostic configuration tests: 16 passed.
- Real installed Nsight, tracing/sampling/context-switch collection disabled:
  reproduced loss of the old empty argument, then verified the complete repaired
  target argv including paths with spaces through a real child.
- Packaged OpenMW directly accepted the repaired argv with --version; no game,
  save, or normal configuration was loaded by this check.
- This does not validate elevated CPU sampling, F12 capture, gameplay, or FPS.
- No C++ changed, so no additional engine build was needed.

## Files

Source edits:
- tools/v4/cp4/diagnosticconfig.py
- tools/v4/cp4/gameplay-diagnostics.py
- tools/v4/cp4/test-diagnostic-config.py
- tools/v4/cp4/test-gameplay-diagnostics.py
- tools/v4/cp4/test-nsight-argv.py (new real process transport test)
- tools/v4/cp4/CP4F-PROFILE-ARGV-HOTFIX.md (this note)

The two production helpers are patched in the existing
build/stage-cp4f-picking-index-20260923-164330 package. Its openmw.exe remains
SHA256 2f8f833c1295934a74f91ee06dda6308cbd60dfcefc37adc85257bfc25ea999e.
Start-CP4F-Profile.cmd is unchanged. Close the failed starter and run the same CMD
as administrator again. The old build manifest and failed capture are retained
as historical evidence; profile-argv-hotfix-manifest.json records the helper
overlay over that original package. Normal configuration and saves are untouched.
