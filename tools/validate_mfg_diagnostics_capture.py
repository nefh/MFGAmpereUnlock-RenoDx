#!/usr/bin/env python3
"""Validate MFG Diagnostics schema-2 capture bundles without replaying DLSS-G."""
from __future__ import annotations

import argparse
import json
import re
import tempfile
from pathlib import Path

SHA256 = re.compile(r"^[0-9a-f]{64}$")
LIFECYCLE_EVENTS = {
    "ngx_fg_feature_created", "ngx_fg_feature_released", "fg_zero_handle_boundary",
    "fg_tracking_lost", "fg_mode_off_observed", "fg_mode_on_observed",
    "quality_change_requested", "quality_change_superseded", "quality_configuration_prepared",
    "quality_restart_required", "live_options_reapplied",
}


def validate_lifecycle(row: dict) -> list[str]:
    event = row.get("lifecycle")
    if not isinstance(event, dict):
        return ["missing lifecycle payload"]
    errors = []
    for key in ("epoch", "handles", "evaluates_in_flight", "creates_in_flight", "releases_in_flight"):
        value = event.get(key)
        if value is not None and (type(value) is not int or value < 0):
            errors.append(f"invalid lifecycle {key}")
    if event.get("tracking_uncertain") is not None and type(event["tracking_uncertain"]) is not bool:
        errors.append("invalid tracking confidence")
    if event.get("cuda_modules_retired") is not None:
        errors.append("NGX lifecycle does not prove CUDA module retirement")
    if row.get("event") == "fg_zero_handle_boundary" and (
            event.get("handles") != 0 or event.get("tracking_uncertain") is not False):
        errors.append("zero-handle boundary lacks complete tracking evidence")
    return errors



def load_json(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def validate(bundle: Path) -> list[str]:
    errors: list[str] = []
    summary_path = bundle / "summary.json"
    trace_path = bundle / "trace.jsonl"
    if not summary_path.is_file():
        errors.append("missing summary.json")
    if not trace_path.is_file():
        errors.append("missing trace.jsonl")
    if errors:
        return errors

    summary = load_json(summary_path)
    if summary.get("schema") != 2:
        errors.append("summary schema must be 2")
    for key in ("evaluate_begin", "evaluate_end", "generated_evaluations",
                "source_evaluations", "reset_evaluations", "unknown_evaluations",
                "host_present_events", "backend_support"):
        if key not in summary:
            errors.append(f"summary missing {key}")

    previous_sequence = 0
    evaluate_begin: set[int] = set()
    evaluate_end: set[int] = set()
    observed = {
        "evaluate_begin": 0,
        "evaluate_end": 0,
        "generated_evaluations": 0,
        "source_evaluations": 0,
        "reset_evaluations": 0,
        "unknown_evaluations": 0,
        "host_present_events": 0,
    }
    with trace_path.open("r", encoding="utf-8") as handle:
        for line_number, raw in enumerate(handle, 1):
            if not raw.strip():
                continue
            try:
                row = json.loads(raw)
            except json.JSONDecodeError as error:
                errors.append(f"trace line {line_number}: {error}")
                continue
            if row.get("schema") != 2:
                errors.append(f"trace line {line_number}: schema must be 2")
            sequence = row.get("sequence")
            if not isinstance(sequence, int) or sequence <= previous_sequence:
                errors.append(f"trace line {line_number}: sequence is not strictly increasing")
            elif sequence:
                previous_sequence = sequence
            if row.get("event") == "ngx_evaluate":
                evaluate = row.get("evaluate") or {}
                evaluation_id = evaluate.get("evaluation_id")
                if not isinstance(evaluation_id, int) or evaluation_id <= 0:
                    errors.append(f"trace line {line_number}: invalid evaluation_id")
                    continue
                phase = evaluate.get("phase")
                if phase == "begin":
                    evaluate_begin.add(evaluation_id)
                    observed["evaluate_begin"] += 1
                elif phase == "end":
                    evaluate_end.add(evaluation_id)
                    observed["evaluate_end"] += 1
                else:
                    errors.append(f"trace line {line_number}: invalid Evaluate phase")
                kind = evaluate.get("frame_kind")
                if kind not in {"unknown", "source", "generated", "reset"}:
                    errors.append(f"trace line {line_number}: invalid frame_kind")
                marker = evaluate.get("marker_label")
                if kind == "generated":
                    index = evaluate.get("generated_index")
                    count = evaluate.get("generated_count")
                    if not isinstance(index, int) or not isinstance(count, int) or \
                            index <= 0 or count <= 0 or index > count:
                        errors.append(f"trace line {line_number}: invalid generated index/count")
                    elif marker != f"FG {index}/{count}":
                        errors.append(f"trace line {line_number}: generated marker does not match index/count")
                elif marker is not None:
                    errors.append(f"trace line {line_number}: marker on non-generated event")
                if evaluate.get("displayed") is not None:
                    errors.append(f"trace line {line_number}: provider Evaluate must not claim display")
                if phase == "begin":
                    if kind == "generated":
                        observed["generated_evaluations"] += 1
                    elif kind == "source":
                        observed["source_evaluations"] += 1
                    elif kind == "reset":
                        observed["reset_evaluations"] += 1
                    else:
                        observed["unknown_evaluations"] += 1
            elif row.get("event") in LIFECYCLE_EVENTS:
                errors.extend(f"trace line {line_number}: {error}" for error in validate_lifecycle(row))
            elif row.get("event") == "quality_reconfigure_commit":
                errors.append(f"trace line {line_number}: this implementation cannot claim a kernel reload")
            elif row.get("event") == "host_present":
                observed["host_present_events"] += 1
    if not evaluate_end.issubset(evaluate_begin):
        errors.append("trace contains Evaluate end without matching begin")
    for key, value in observed.items():
        if summary.get(key) != value:
            errors.append(f"summary {key}={summary.get(key)!r} does not match trace value {value}")

    manifest_path = bundle / "capture_manifest.json"
    if manifest_path.is_file():
        manifest = load_json(manifest_path)
        if manifest.get("schema") != 2:
            errors.append("capture manifest schema must be 2")
        begin = manifest.get("evaluate_begin") or {}
        end = manifest.get("evaluate_end") or {}
        if begin.get("evaluation_id") != end.get("evaluation_id"):
            errors.append("capture manifest Evaluate IDs do not match")
        sha = (manifest.get("provider") or {}).get("sha256")
        if sha is not None and (not isinstance(sha, str) or not SHA256.fullmatch(sha)):
            errors.append("capture manifest provider SHA256 is invalid")
        if manifest.get("resource_contents_captured") is not False:
            errors.append("current schema expects metadata-only resource capture")
        for phase_name in ("evaluate_begin", "evaluate_end"):
            phase = manifest.get(phase_name) or {}
            for resource in phase.get("resources") or []:
                if resource.get("readback_available") is not False:
                    errors.append(f"capture manifest {phase_name} resource unexpectedly claims readback")
        replay = manifest.get("replay") or {}
        if replay.get("status") != "gated":
            errors.append("current metadata-only capture must keep replay gated")
        if not replay.get("missing"):
            errors.append("gated replay must list missing requirements")
    return errors


def self_test() -> int:
    row = {"event": "fg_zero_handle_boundary", "lifecycle": {
        "handles": 0, "tracking_uncertain": False, "cuda_modules_retired": None}}
    assert not validate_lifecycle(row)
    row["lifecycle"]["handles"] = 1
    assert validate_lifecycle(row)
    row["lifecycle"]["handles"] = 0
    row["lifecycle"]["cuda_modules_retired"] = True
    assert validate_lifecycle(row)
    row["lifecycle"]["cuda_modules_retired"] = None
    row["lifecycle"]["tracking_uncertain"] = None
    assert validate_lifecycle(row)
    with tempfile.TemporaryDirectory() as temporary:
        bundle = Path(temporary)
        summary = {
            "schema": 2,
            "evaluate_begin": 1,
            "evaluate_end": 1,
            "generated_evaluations": 1,
            "source_evaluations": 0,
            "unknown_evaluations": 0,
            "reset_evaluations": 0,
            "host_present_events": 1,
            "backend_support": {"d3d12": "metadata"},
        }
        (bundle / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
        rows = [
            {"schema": 2, "event": "ngx_evaluate", "sequence": 1,
             "evaluate": {"evaluation_id": 9, "phase": "begin",
                          "frame_kind": "generated", "marker_label": "FG 1/1",
                          "generated_index": 1, "generated_count": 1, "displayed": None}},
            {"schema": 2, "event": "ngx_evaluate", "sequence": 2,
             "evaluate": {"evaluation_id": 9, "phase": "end",
                          "frame_kind": "generated", "marker_label": "FG 1/1",
                          "generated_index": 1, "generated_count": 1, "displayed": None}},
            {"schema": 2, "event": "host_present", "sequence": 3},
        ]
        (bundle / "trace.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        manifest = {
            "schema": 2,
            "provider": {"sha256": "0" * 64},
            "evaluate_begin": {"evaluation_id": 9, "resources": []},
            "evaluate_end": {"evaluation_id": 9, "resources": []},
            "resource_contents_captured": False,
            "replay": {"status": "gated", "missing": ["resource contents"]},
        }
        (bundle / "capture_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        errors = validate(bundle)
        if errors:
            print("self_test=FAIL")
            for error in errors:
                print(error)
            return 1

        bad = json.loads((bundle / "capture_manifest.json").read_text(encoding="utf-8"))
        bad["replay"]["status"] = "implemented"
        (bundle / "capture_manifest.json").write_text(json.dumps(bad), encoding="utf-8")
        if not validate(bundle):
            print("self_test=FAIL: gated replay regression not detected")
            return 1
    print("self_test=PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.bundle is None:
        parser.error("bundle path is required unless --self-test is used")
    errors = validate(args.bundle)
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 2
    print("capture_validation=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
