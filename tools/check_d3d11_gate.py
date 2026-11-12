#!/usr/bin/env python3
"""Verdicts for the D3D11 regression gates.

The scenario files describe captures and timelines only. This checker holds the
per-gate conditions the captures cannot express, so every gate can still fail on
its own subsystem:

  d3d11-cleared-frames     result pass, draw_calls > 0, sustained presentation,
                           zero device removals, non-uniform captures
  d3d11-texture-uploads    result pass, uploads per presented frame, zero upload-path
                           failures, draw_calls > 0, non-uniform captures
  d3d11-native-resolve-path result pass, resolve tally with no skips,
                           draw_calls > 0, non-uniform captures
  d3d11-draws              result pass, draw_calls > 0, zero device removals,
                           non-uniform captures

Usage: check_d3d11_gate.py <scenario name> [--run <run dir>]
Exit code 0 = gate passed, 1 = gate failed.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

AUTOMATED = Path("E:/PinyonShift/work/.local/native-renderer/automated")

# Presentation floor for the sustained-presentation gate. The pre-draw build
# held ~60 Hz with a cleared frame; with the draw path and the per-swap GPU
# wait live the measured median is ~40 Hz on the GTX 570, so the floor asserts
# sustained motion rather than the empty-frame cadence (Job D quantifies the
# wait's cost).
MIN_PRESENT_HZ = 30.0
MAX_MEDIAN_FRAME_US = 40000.0
# Uploads are graded per frame, not as a run total: the scenario is wall-clock
# bounded, so a slower but correct frame path does less total work in the same
# 70 s and failed a floor calibrated on the faster path (executor+readback:
# 10,000 uploads over 2,132 frames). The upload mix is invariant across speed -
# 8 runs spanning 1,968-3,425 frames measured 4.58-4.83 uploads per frame,
# including 4.69 on that executor run. The floor keeps ~15% below the slowest
# observed mix and leans on zero upload-path failures for the rest of its
# meaning; the small absolute floor only guards against a near-empty run.
MIN_UPLOADS_PER_FRAME = 4.0
MIN_UPLOAD_TALLY = 500

DEVICE_REMOVAL_PATTERNS = ("887A0005", "DEVICE_REMOVED", "device removed")
UPLOAD_FAILURE_PATTERNS = (
    "D3D11 texture cache: CreateTexture2D failed",
    "D3D11 texture cache: CreateTexture3D failed",
    "D3D11 texture cache: CreateShaderResourceView failed",
)


def newest_run(scenario: str) -> Path:
    runs = sorted(
        (path for path in AUTOMATED.glob(f"{scenario}-*") if not path.name.endswith(".state")),
        key=lambda path: path.name,
    )
    if not runs:
        raise SystemExit(f"FAIL {scenario}: no run directory")
    return runs[-1]


def runtime_log(run: Path) -> str:
    # Every rotated segment, oldest first: the run's start lives in the oldest.
    logs = run.with_name(run.name + ".state") / "logs"
    segments = sorted(
        logs.glob("runtime.log*"), key=lambda path: (path.name != "runtime.log", path.name)
    )
    chunks = []
    for segment in segments:
        chunks.append(segment.read_bytes().replace(b"\x00", b"").decode("utf-8", "replace"))
    return "\n".join(chunks)


def last_int(pattern: str, text: str) -> int | None:
    matches = re.findall(pattern, text)
    if not matches:
        return None
    return int(matches[-1])


def fail(reason: str) -> int:
    print(f"FAIL {reason}")
    return 1


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_d3d11_gate.py <scenario> [--run <dir>]")
        return 2
    scenario = sys.argv[1].removesuffix(".fh1test")
    if "--run" in sys.argv:
        run = Path(sys.argv[sys.argv.index("--run") + 1])
    else:
        run = newest_run(scenario)

    result_path = run / "result.json"
    if not result_path.exists():
        return fail(f"{scenario}: no result.json in {run.name} (the run aborted)")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("result") != "pass":
        return fail(f"{scenario}: runner result {result.get('result')!r}")

    log = runtime_log(run)
    if not log:
        return fail(f"{scenario}: empty runtime log in {run.name}")

    captures = result.get("captures") or []
    if not captures:
        return fail(f"{scenario}: no captures recorded")
    for capture in captures:
        span = int(capture["maximum"]) - int(capture["minimum"])
        if span < 8:
            return fail(
                f"{scenario}: capture {Path(capture['file']).name} is uniform "
                f"(byte span {span}) - the presented frame is blank"
            )

    for pattern in DEVICE_REMOVAL_PATTERNS:
        if pattern in log:
            return fail(f"{scenario}: device removal ({pattern}) in {run.name}")

    counters = (result.get("performance") or {}).get("frames", {}).get("counter_totals", {})
    draw_calls = int(counters.get("draw_calls") or 0)
    if draw_calls <= 0:
        return fail(f"{scenario}: draw_calls is {draw_calls}")

    if scenario == "d3d11-cleared-frames":
        cadence = (result["performance"].get("presentation") or {}).get("cadence_hz", {})
        present = float(cadence.get("present") or 0.0)
        median_us = float(result["performance"]["frames"]["frame_time_us"]["median"])
        if present < MIN_PRESENT_HZ:
            return fail(f"{scenario}: present cadence {present:.2f} Hz below {MIN_PRESENT_HZ}")
        if median_us > MAX_MEDIAN_FRAME_US:
            return fail(f"{scenario}: median frame time {median_us:.0f} us above "
                        f"{MAX_MEDIAN_FRAME_US:.0f} us")
        detail = f"present {present:.2f} Hz, median {median_us:.0f} us"

    elif scenario == "d3d11-texture-uploads":
        tally = last_int(r"uploaded (\d+) textures", log)
        if tally is None:
            return fail(f"{scenario}: no upload tally in the runtime log")
        # The last swap the scenario issued: the denominator the tally is a rate
        # over. A slower run reaches fewer frames, and the rate stays the same.
        frames = last_int(r"D3D11 draws frame=(\d+)", log)
        if frames is None or frames <= 0:
            return fail(f"{scenario}: no frame tally in the runtime log")
        uploads_per_frame = tally / frames
        if tally < MIN_UPLOAD_TALLY or uploads_per_frame < MIN_UPLOADS_PER_FRAME:
            return fail(
                f"{scenario}: uploads {tally} over {frames} frames is "
                f"{uploads_per_frame:.2f} per frame, below {MIN_UPLOADS_PER_FRAME}"
            )
        for pattern in UPLOAD_FAILURE_PATTERNS:
            if pattern in log:
                return fail(f"{scenario}: upload-path failure ({pattern})")
        detail = f"uploads {tally} over {frames} frames ({uploads_per_frame:.2f}/frame)"

    elif scenario == "d3d11-native-resolve-path":
        tallies = re.findall(r"FH1 native resolve: (\d+) dispatches, (\d+) skipped", log)
        if not tallies:
            return fail(f"{scenario}: no resolve tally in the runtime log")
        dispatches, skipped = (int(value) for value in tallies[-1])
        if dispatches <= 0:
            return fail(f"{scenario}: resolve tally reports {dispatches} dispatches")
        if skipped != 0:
            return fail(f"{scenario}: resolve tally reports {skipped} skipped")
        skipped_plans = log.count("FH1 native resolve: skipped (")
        if skipped_plans:
            return fail(f"{scenario}: {skipped_plans} resolve plans skipped")
        detail = f"resolves {dispatches}, 0 skipped"

    elif scenario == "d3d11-draws":
        detail = "presented title frame"

    else:
        detail = "generic checks"

    spans = ", ".join(
        f"{Path(c['file']).stem} [{c['minimum']}-{c['maximum']}]" for c in captures
    )
    print(f"PASS {scenario} ({run.name}): draw_calls={draw_calls}, {detail}; captures {spans}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
