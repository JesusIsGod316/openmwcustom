#!/usr/bin/env python3
"""Qualify asset consumption in one short, completed Phase 9 capture.

Only reads named capture members, never extracts an archive or runs the game.
Exit 0: requested lanes have actual consumption (or this is a control).
Exit 2: complete proof reports no eligible work or no actual consumption.
Exit 3: required proof is missing, malformed, conflicting, or inconsistent.
Exit 4: the game crashed/failed; partial counters cannot qualify a release.
This is an engagement gate, not image-quality or performance acceptance.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import pathlib
import re
import sys
import zipfile
from contextlib import contextmanager

SCHEMA = 1
LANES = {1: "L1", 2: "H1", 4: "T1", 8: "T2"}
REQUIRED = {
    "L1": ("requested", "eligible", "built", "original_indices", "derived_indices",
           "consumed_draws", "consumed_indices"),
    "H1": ("requested", "eligible", "built", "control_submissions", "proxy_submissions",
           "consumed_draws", "consumed_indices"),
    "T1": ("requested", "eligible", "ready", "native_images", "consumed_main_draws", "consumed_composite_draws"),
    "T2": ("requested", "eligible", "boundaries", "allocations", "uploads", "upload_bytes",
           "coarse_published", "fine_published", "coarse_draws", "fine_draws", "demotions",
           "deletions", "live_bytes", "peak_bytes"),
}
QC_LINE = re.compile(r"OptimizedMW asset QC (v\d+|L1|H1|T1|T2)( refusals)?:\s*(.*?)\s*$")
INTEGER = re.compile(r"0|[1-9][0-9]*")
KEY = re.compile(r"[a-z][a-z0-9_]*")
SHA = re.compile(r"[0-9a-fA-F]{64}")
COMMIT = re.compile(r"[0-9a-fA-F]{40}")
MAX_MEMBER = 64 * 1024 * 1024


class EvidenceError(ValueError):
    pass


def strict_json(text, allow_nonfinite=False):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise EvidenceError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result
    def constant(value):
        raise EvidenceError(f"nonfinite JSON constant {value}")
    result = json.loads(text, object_pairs_hook=pairs, **({} if allow_nonfinite else {"parse_constant": constant}))
    if not isinstance(result, dict):
        raise EvidenceError("capture JSON root must be an object")
    return result


def decimal(value, name):
    # bool is an int in Python; it is not a retained integer counter.
    if isinstance(value, bool) or not INTEGER.fullmatch(str(value)):
        raise EvidenceError(f"{name}: expected an unsigned decimal integer, got {value!r}")
    result = int(value)
    if result > (1 << 64) - 1:
        raise EvidenceError(f"{name}: exceeds uint64")
    return result


def key_values(text):
    result = {}
    for token in text.split():
        if token.count("=") != 1:
            raise EvidenceError(f"malformed counter token {token!r}")
        key, value = token.split("=", 1)
        if not KEY.fullmatch(key) or key in result:
            raise EvidenceError(f"invalid or duplicate counter key {key!r}")
        result[key] = decimal(value, key)
    if not result:
        raise EvidenceError("empty counter/refusal report")
    return result


def parse_normal_proof(log):
    """Never substitutes requested settings or optional diagnostics for proof."""
    proof = {"header": None, "lanes": {}, "refusals": {}, "source_lines": {}, "errors": []}
    for number, line in enumerate(log.splitlines(), 1):
        if "OptimizedMW asset QC " not in line:
            continue
        match = QC_LINE.search(line)
        if not match:
            proof["errors"].append(f"openmw.log:{number}: malformed asset QC line")
            continue
        kind, refusal, text = match.groups()
        name = kind + (" refusals" if refusal else "")
        if name in proof["source_lines"]:
            proof["errors"].append(f"openmw.log:{number}: duplicate {name}; a final snapshot must be unique")
            continue
        proof["source_lines"][name] = number
        try:
            values = key_values(text)
        except EvidenceError as error:
            proof["errors"].append(f"openmw.log:{number}: {error}")
            continue
        if kind.startswith("v"):
            if kind != "v1" or refusal:
                proof["errors"].append(f"openmw.log:{number}: unsupported QC schema {name}")
            elif proof["header"] is not None:
                proof["errors"].append(f"openmw.log:{number}: duplicate QC header")
            else:
                proof["header"] = values
        else:
            proof["refusals" if refusal else "lanes"][kind] = values
    return proof


def evaluate_lane(lane, counts, refusals, worker_frozen=False):
    result = {"lane": lane, "status": "INCOMPLETE_EVIDENCE", "actually_engaged": False,
              "counters": counts, "refusals": refusals, "problems": []}
    if counts is None:
        result["problems"].append(f"missing Normal {lane} counter report")
    else:
        missing = [field for field in REQUIRED[lane] if field not in counts]
        if missing:
            result["problems"].append("missing required counters: " + ", ".join(missing))
    if refusals is None:
        result["problems"].append(f"missing Normal {lane} refusal report; absence is not zero")
    if result["problems"]:
        return result
    c = counts
    if worker_frozen and c["eligible"] > c["requested"]:
        result["problems"].append("eligible exceeds its same-stage requested denominator")
    if lane in ("L1", "H1"):
        if c["built"] == 0 and (c["consumed_draws"] or c["consumed_indices"]):
            result["problems"].append("draw receipt without any built representation")
        if bool(c["consumed_draws"]) != bool(c["consumed_indices"]):
            result["problems"].append("draw/index receipt populations disagree")
        if c["built"]:
            left, right = ("original_indices", "derived_indices") if lane == "L1" else ("control_submissions", "proxy_submissions")
            if not 0 < c[right] < c[left]:
                result["problems"].append(f"built representation has no positive {left}/{right} reduction")
        consumed = c["consumed_draws"]
        prepared = c["built"]
    elif lane == "T1":
        consumed, prepared = c["consumed_main_draws"] + c["consumed_composite_draws"], c["ready"]
        result["consumer_scope"] = ("MAIN_DRAW_AND_COMPOSITE_SOURCE" if c["consumed_main_draws"] and c["consumed_composite_draws"]
                                    else "MAIN_DRAW" if c["consumed_main_draws"] else "COMPOSITE_SOURCE_ONLY" if c["consumed_composite_draws"] else "NONE")
        result["quality_scope"] = "FULL_NATIVE_BACKING_FIRST_MIP_ZERO; coarse-only T2 source use does not qualify this T1 full-quality receipt"
        if consumed and (not prepared or not c["native_images"]):
            result["problems"].append("main draw receipt without ready native image adoption")
    else:
        consumed = c["coarse_draws"] + c["fine_draws"]
        prepared = c["coarse_published"] + c["fine_published"]
        if c["live_bytes"] > c["peak_bytes"]:
            result["problems"].append("live candidate allocation debt exceeds retained peak")
        if c["coarse_draws"] and not c["coarse_published"]:
            result["problems"].append("coarse draw without coarse publication")
        if c["fine_draws"] and not c["fine_published"]:
            result["problems"].append("fine draw without fine publication")
        if prepared and not (c["allocations"] and c["uploads"] and c["upload_bytes"] and c["boundaries"]):
            result["problems"].append("publication without owner-boundary allocation/upload proof")
    if consumed and not c["eligible"]:
        result["problems"].append("actual consumption without any eligible producer")
    if result["problems"]:
        result["status"] = "INCONSISTENT_EVIDENCE"
    elif consumed:
        result["status"], result["actually_engaged"] = "ENGAGED", True
        if lane == "T1" and result["consumer_scope"] == "COMPOSITE_SOURCE_ONLY":
            result["status"] = "ENGAGED_COMPOSITE_SOURCE_ONLY"
    elif not c["eligible"]:
        # The outer optional stage can decline before any original source is
        # certified. Zero eligibility then proves neither natural ineligibility
        # nor an empty route; retain the actual gate as the diagnosis.
        if refusals.get("outer_cpu_denied", 0):
            result["status"] = "ELIGIBILITY_BLOCKED"
            result["eligibility_evidence"] = "ADMISSION_BLOCKED_BEFORE_SOURCE_CERTIFICATION"
            result["problems"].append("outer CPU admission declined before source eligibility was established; zero eligible is not evidence of a naturally ineligible route")
        else:
            result["status"] = "NO_ELIGIBLE_WORK"
            result["eligibility_evidence"] = ("SOURCE_REQUESTS_WITHOUT_CERTIFICATION_PROOF"
                if c["requested"] and not refusals.get("source_rejected", 0) else "NO_ELIGIBLE_PRODUCER_OBSERVED")
            result["problems"].append("route did not prove eligible work; this does not establish a defective pipeline or natural ineligibility without source/refusal evidence")
    elif prepared:
        result["status"] = "PREPARED_NOT_CONSUMED"
        result["problems"].append("prepared output was never consumed; readiness/build counts cannot qualify the candidate")
    else:
        result["status"] = "ELIGIBLE_NOT_PREPARED"
        result["problems"].append("eligible requests produced no ready representation; inspect retained refusals/admission/cutoff")
    return result


class Capture:
    """Bounded reads from exact member names; no extraction or directory traversal."""
    def __init__(self, path):
        self.path = pathlib.Path(path).resolve()
        self.zip = zipfile.ZipFile(self.path) if self.path.is_file() else None
        if self.zip:
            names = self.zip.namelist()
            if len(set(names)) != len(names):
                self.zip.close()
                raise EvidenceError("ZIP contains duplicate member names")
            self.names = set(names)
        elif self.path.is_dir():
            self.names = {p.name for p in self.path.iterdir() if p.is_file()}
        else:
            raise EvidenceError("capture must be an existing ZIP or directory")

    def close(self):
        if self.zip:
            self.zip.close()

    @contextmanager
    def text(self, name):
        if name not in self.names:
            raise EvidenceError(f"missing capture member {name}")
        size = self.zip.getinfo(name).file_size if self.zip else (self.path / name).stat().st_size
        if size > MAX_MEMBER:
            raise EvidenceError(f"{name} exceeds bounded {MAX_MEMBER}-byte input limit")
        stream = self.zip.open(name) if self.zip else (self.path / name).open("rb")
        with io.TextIOWrapper(stream, encoding="utf-8-sig", errors="strict") as decoded:
            yield decoded

    def read(self, name):
        with self.text(name) as stream:
            return stream.read()

    def sha256(self, name):
        if name not in self.names:
            raise EvidenceError(f"missing capture member {name}")
        size = self.zip.getinfo(name).file_size if self.zip else (self.path / name).stat().st_size
        if size > MAX_MEMBER:
            raise EvidenceError(f"{name} exceeds bounded input limit")
        stream = self.zip.open(name) if self.zip else (self.path / name).open("rb")
        digest = hashlib.sha256()
        with stream:
            while chunk := stream.read(65536):
                digest.update(chunk)
        return digest.hexdigest()

    def json(self, name, allow_nonfinite=False):
        return strict_json(self.read(name), allow_nonfinite)


def verify_active_capture(capture):
    # Historical PowerShell reports can emit NaN for empty, unrelated cohorts.
    # Only the checked integer active count is consumed here; no NaN metric is
    # copied into our JSON or accepted as frame/state proof.
    report = capture.json("Phase9-BENCHMARK-REPORT.json", allow_nonfinite=True)
    context = report.get("gameplay_context", {})
    if context.get("qualified") is not True:
        raise EvidenceError("frame/context report is not qualified")
    expected = context.get("observed_state_cohorts", {}).get("cohorts", {}).get("observed_active", {}).get("frames")
    expected = decimal(expected, "reported observed-active frames")
    with capture.text("benchmark-context.csv.capture-status.txt") as stream:
        status = {}
        for line in stream:
            if "=" not in line:
                continue
            key, value = line.rstrip("\r\n").split("=", 1)
            if key in status:
                raise EvidenceError("duplicate benchmark context writer-status key")
            status[key] = value
    for key, value in {"schema": "1", "normal_finish": "1", "output_ok": "1", "allocation_failed": "0",
                       "dropped": "0", "snapshots_dropped": "0", "unfinished_frames": "0"}.items():
        if status.get(key) != value:
            raise EvidenceError(f"benchmark context status {key} is missing or not {value}")
    states = {}
    with capture.text("benchmark-context.csv") as stream:
        for row in csv.DictReader(stream):
            if not row.get("frame") or row["frame"].startswith("#"):
                continue
            frame = decimal(row["frame"], "context frame")
            if frame in states:
                raise EvidenceError("duplicate context frame")
            flags = decimal(row.get("observed_flags"), "observed_flags")
            if flags & ~127:
                raise EvidenceError("unknown state-flag bit")
            states[frame] = flags
    if decimal(status.get("rows"), "context status rows") != len(states):
        raise EvidenceError("context writer row count differs from retained rows")
    seen, active = set(), 0
    with capture.text("v3-frame.csv") as stream:
        for row in csv.DictReader(stream):
            if not row.get("frame") or row["frame"].startswith("#"):
                continue
            frame = decimal(row["frame"], "engine frame")
            if frame in seen or frame not in states:
                raise EvidenceError("duplicate frame or missing state receipt")
            wall = float(row["wall_ms"])
            if not math.isfinite(wall) or wall < 0:
                raise EvidenceError("invalid engine wall time")
            seen.add(frame)
            active += states[frame] == 0
    if seen != states.keys() or active != expected or not active:
        raise EvidenceError("engine/state join, reported active count, or positive gameplay exposure failed")
    return {"frames": len(seen), "observed_active_frames": active,
            "meaning": "OR-observed state-qualified engine frames, not identical route or display FPS"}


def historical_summary(log):
    # Retain old evidence for diagnosis, never upgrade it to v1 completeness.
    patterns = ("OptimizedMW asset pipeline:", "OptimizedMW HLOD:", "OptimizedMW texture streaming:")
    result = []
    for number, line in enumerate(log.splitlines(), 1):
        if any(prefix in line for prefix in patterns):
            result.append({"member": "openmw.log", "line": number, "text": line})
    return result[-6:]


def verify_capture(path, expected_source=None, expected_exe=None):
    result = {"schema": SCHEMA, "capture": str(pathlib.Path(path).resolve()), "status": "INCOMPLETE_EVIDENCE",
              "exit_code": 3, "actually_engaged": False, "performance_acceptance": "NOT_ASSESSED",
              "visual_acceptance": "NOT_ASSESSED", "fixture_acceptance": "SEPARATE_RELEASE_GATE",
              "problems": [], "lanes": {}}
    capture = None
    try:
        capture = Capture(path)
        manifest_text = capture.read("RUN-MANIFEST.json")
        manifest = strict_json(manifest_text)
        result["manifest_sha256"] = capture.sha256("RUN-MANIFEST.json")
        result["source_commit"] = manifest.get("engine_source_commit_from_build_identity")
        result["executable_sha256"] = manifest.get("executable", {}).get("sha256")
        exit_code = manifest.get("completion", {}).get("exit_code")
        result["game_exit_code"] = exit_code
        log = capture.read("openmw.log") if "openmw.log" in capture.names else ""
        result["historical_partial_summaries"] = historical_summary(log)
        if exit_code is not None and (isinstance(exit_code, bool) or not isinstance(exit_code, int)):
            raise EvidenceError("completion.exit_code must be a retained integer, not a label")
        if (exit_code is not None and exit_code != 0) or "openmw-crash.dmp" in capture.names:
            result.update(status="FAILED_GAME", exit_code=4)
            result["problems"].append("crash/nonzero game exit: partial/missing shutdown proof cannot qualify a release")
            return result
        if exit_code != 0:
            raise EvidenceError("game completion is missing; a pending launch cannot qualify")
        if not COMMIT.fullmatch(str(result["source_commit"])) or not SHA.fullmatch(str(result["executable_sha256"])):
            raise EvidenceError("exact source/executable identity is missing")
        if expected_source and result["source_commit"].lower() != expected_source.lower():
            raise EvidenceError("capture source differs from required release source")
        if expected_exe and result["executable_sha256"].lower() != expected_exe.lower():
            raise EvidenceError("capture executable differs from required release executable")
        mask = decimal(manifest.get("asset_pipeline", {}).get("mask"), "manifest asset mask")
        result["requested_mask"] = mask
        if mask > 31 or (mask & 16 and not mask & 8):
            raise EvidenceError("unknown asset bit or demotion requested without streaming")
        mode_mask = manifest.get("phase9_mode", {}).get("AssetMask")
        if mode_mask is not None and decimal(mode_mask, "mode AssetMask") != mask:
            raise EvidenceError("manifest asset mask conflicts with its requested mode")
        applied = manifest.get("launcher_applied_openmw_environment", {}).get("OPENMW_P9_ASSET_MASK")
        if applied is not None and decimal(applied, "applied asset mask") != mask:
            raise EvidenceError("manifest mask conflicts with launcher-applied environment")
        result["gameplay_exposure"] = verify_active_capture(capture)
        proof = parse_normal_proof(log)
        result["normal_proof"] = proof
        result["problems"].extend(proof["errors"])
        if proof["header"] is None:
            if mask == 0 and not proof["errors"]:
                result.update(status="CONTROL_REFERENCE", exit_code=0)
                result["problems"].append("control requests no candidate; no engagement claim is made")
                return result
            raise EvidenceError("missing versioned Normal final proof; old summary, startup flags and build/ready totals are insufficient")
        header = proof["header"]
        if header.get("final") != 1 or header.get("mask") != mask or header.get("draw_stopped") != 1:
            raise EvidenceError("Normal final marker or actual runtime mask does not match the completed capture")
        if header.get("worker_frozen") not in (0, 1):
            raise EvidenceError("worker_frozen must explicitly distinguish loop-end observations from a frozen producer snapshot")
        result["actual_mask"] = header["mask"]
        result["snapshot_semantics"] = "draw counters stopped; producer counters frozen" if header["worker_frozen"] else "draw counters stopped; producer counters are nontransactional loop-end observations"
        for bit, lane in LANES.items():
            if mask & bit:
                result["lanes"][lane] = evaluate_lane(lane, proof["lanes"].get(lane), proof["refusals"].get(lane), bool(header["worker_frozen"]))
        for lane in proof["lanes"]:
            if lane not in result["lanes"]:
                # Disabled-lane zero reports are harmless. Nonzero consumption
                # cannot be silently attributed to the requested configuration.
                draw_keys = ("consumed_draws", "consumed_main_draws", "consumed_composite_draws", "coarse_draws", "fine_draws", "demotion_draws")
                if any(proof["lanes"][lane].get(key, 0) for key in draw_keys):
                    result["problems"].append(f"disabled {lane} reports actual candidate consumption")
        if mask & 16:
            c = proof["lanes"].get("T2", {})
            d = {"lane": "DEMOTION", "status": "INCOMPLETE_EVIDENCE", "actually_engaged": False,
                 "counters": {k: c.get(k) for k in ("demotion_eligible", "demotions", "demotion_draws")}, "problems": []}
            if "demotion_eligible" not in c or "demotion_draws" not in c or "demotions" not in c:
                d["problems"].append("missing demotion eligibility/draw receipts; ordinary coarse draws do not prove a demoted backing was consumed")
            elif c["demotion_draws"] and not (c["demotions"] and c["demotion_eligible"]):
                d["status"] = "INCONSISTENT_EVIDENCE"
                d["problems"].append("demotion draw without eligible demotion/publication")
            elif c["demotion_draws"]:
                d.update(status="ENGAGED", actually_engaged=True)
            elif not c["demotion_eligible"]:
                d["status"] = "NO_ELIGIBLE_WORK"
                d["problems"].append("no qualified inactive fine backing on this smoke; no demotion performance claim")
            else:
                d["status"] = "PREPARED_NOT_CONSUMED" if c["demotions"] else "ELIGIBLE_NOT_PREPARED"
                d["problems"].append("eligible demotion has no actual replacement draw receipt")
            result["lanes"]["DEMOTION"] = d
        bad_proof = any(v["status"] in ("INCOMPLETE_EVIDENCE", "INCONSISTENT_EVIDENCE") for v in result["lanes"].values())
        zero = any(not v["actually_engaged"] for v in result["lanes"].values())
        if result["problems"] or bad_proof:
            result.update(status="INCOMPLETE_EVIDENCE", exit_code=3)
        elif zero:
            result.update(status="NOT_QUALIFIED_FOR_CANDIDATE_COMPARISON", exit_code=2)
        elif mask:
            result.update(status="ENGAGEMENT_QUALIFIED", exit_code=0, actually_engaged=True)
        else:
            result.update(status="CONTROL_REFERENCE", exit_code=0)
    except (EvidenceError, OSError, ValueError, TypeError, KeyError, AttributeError, zipfile.BadZipFile, UnicodeError) as error:
        result["problems"].append(str(error))
    finally:
        if capture:
            capture.close()
    return result


def report_text(result):
    lines = ["PHASE 9 SHORT-CAPTURE ASSET ENGAGEMENT GATE", f"Capture: {result['capture']}",
             f"Outcome: {result['status']} (exit {result['exit_code']})",
             f"Actually engaged: {result['actually_engaged']}",
             "This gate does not grant visual/gameplay/performance acceptance."]
    for key in ("requested_mask", "actual_mask", "source_commit", "executable_sha256"):
        if key in result:
            lines.append(f"{key}: {result[key]}")
    if "gameplay_exposure" in result:
        lines.append("State-qualified observed-active frames: " + str(result["gameplay_exposure"]["observed_active_frames"]))
    for problem in result["problems"]:
        lines.append("Evidence: " + problem)
    for lane in result["lanes"].values():
        lines.append(f"{lane['lane']}: {lane['status']}; actual consumption={lane['actually_engaged']}")
        lines.append("  Counters: " + json.dumps(lane["counters"], sort_keys=True))
        if "consumer_scope" in lane:
            lines.append("  Consumer coverage: " + lane["consumer_scope"] + "; composite-only use does not attest a main-view asset or general FPS mechanism")
        if "quality_scope" in lane:
            lines.append("  Quality coverage: " + lane["quality_scope"])
        if "eligibility_evidence" in lane:
            lines.append("  Eligibility evidence: " + lane["eligibility_evidence"])
        if "refusals" in lane:
            lines.append("  Refusals: " + json.dumps(lane["refusals"], sort_keys=True))
        lines.extend("  " + problem for problem in lane["problems"])
    if result["exit_code"]:
        lines.append("Stop candidate performance comparison. Diagnose this capture's missing/refusal/cutoff evidence; do not require a broad benchmark matrix.")
    lines.append("Refusal counters may count attempts/observations, not distinct assets or disjoint populations. A no-eligible result does not prove a pipeline defect.")
    lines.append("Production fixture quality/stock-shader parity, package identity and user visual acceptance remain separate release gates.")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=pathlib.Path, help="completed short smoke capture ZIP or directory")
    parser.add_argument("--output-dir", type=pathlib.Path, required=True, help="write ASSET-ENGAGEMENT.json and .txt here")
    parser.add_argument("--expected-source", help="require this exact 40-character source commit")
    parser.add_argument("--expected-exe", help="require this exact executable SHA256")
    parser.add_argument("--run-token", help="caller freshness token, retained only to reject stale reports")
    args = parser.parse_args(argv)
    result = verify_capture(args.capture, args.expected_source, args.expected_exe)
    if args.run_token:
        result["run_token"] = args.run_token
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "ASSET-ENGAGEMENT.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    text = report_text(result)
    (args.output_dir / "ASSET-ENGAGEMENT.txt").write_text(text, encoding="utf-8")
    print(text, end="")
    return result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
