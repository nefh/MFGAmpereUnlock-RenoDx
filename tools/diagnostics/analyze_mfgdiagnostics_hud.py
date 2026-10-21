#!/usr/bin/env python3
"""Offline, read-only HUD tag audit for *existing* MFGDiagnostics schema-2 bundles.

Accepts 1-2 bundle directories (trace.jsonl, summary.json). No game hooks,
GPU readbacks, or claims about how generated frames were displayed.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from typing import Any

HUD_ROLES = ("HUDLessColor", "UIAlpha", "UIColorAndAlpha")


def load(bundle: Path) -> dict[str, Any]:
    summary_file = bundle / "summary.json"
    trace_file = bundle / "trace.jsonl"
    if not summary_file.is_file() or not trace_file.is_file():
        raise ValueError(f"Expected summary.json and trace.jsonl in {bundle}")
    summary = json.loads(summary_file.read_text(encoding="utf-8"))
    if summary.get("schema") != 2:
        raise ValueError(f"Unsupported summary schema in {bundle}: {summary.get('schema')}")

    rows = 0
    recognized_hud = 0
    legacy_unclassified = 0
    options = []
    counts: dict[str, Counter] = {role: Counter() for role in HUD_ROLES}
    distinct = {role: set() for role in HUD_ROLES}
    previous_resource: dict[str, int] = {}
    frame_masks: dict[tuple[int, int], set[str]] = defaultdict(set)
    other_counts = Counter()
    previous_seq = 0
    bad_sequence = 0
    with trace_file.open(encoding="utf-8") as reader:
        for lineno, line in enumerate(reader, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            rows += 1
            seq = row.get("sequence")
            if not isinstance(seq, int) or seq <= previous_seq:
                bad_sequence += 1
            if isinstance(seq, int):
                previous_seq = seq
            event = row.get("event")
            other_counts[event] += 1
            if event == "streamline_options":
                option = row.get("options") or {}
                options.append({"sequence": seq, "mode": option.get("mode"),
                                "ui_recomposition": option.get("ui_recomposition"),
                                "generated_requested": option.get("generated_requested")})
                continue
            if event != "resource_tag":
                continue
            tag = row.get("resource_tag") or {}
            role = tag.get("role")
            if role not in HUD_ROLES:
                # Older schema-2 captures have numeric type without role labels.
                if role is None:
                    legacy_unclassified += 1
                continue
            recognized_hud += 1
            counts[role]["observations"] += 1
            pointer_present = tag.get("tag_resource_present")
            # Older logs without this field may have a resource ID, but we must
            # not conflate an unknown native handle with an explicit clear.
            if pointer_present is False:
                counts[role]["clear_tags"] += 1
                continue
            if pointer_present is not True:
                counts[role]["presence_unknown"] += 1
                continue
            counts[role]["active_tags"] += 1
            if row.get("result") not in (None, 0):
                counts[role]["failed_calls"] += 1
                continue
            native = tag.get("resource")
            if native is None:
                counts[role]["native_identity_unavailable"] += 1
            else:
                distinct[role].add(native)
                if role in previous_resource and previous_resource[role] != native:
                    counts[role]["resource_changes"] += 1
                previous_resource[role] = native
            frame = row.get("frame_id")
            viewport = row.get("viewport")
            if frame is None or viewport is None:
                counts[role]["unframed"] += 1
            else:
                frame_masks[(viewport, frame)].add(role)

    frames_with_pair = sum(
        "HUDLessColor" in roles and bool({"UIAlpha", "UIColorAndAlpha"} & roles)
        for roles in frame_masks.values()
    )
    roles_stats = {}
    for role in HUD_ROLES:
        roles_stats[role] = {
            name: int(counts[role][name]) for name in (
                "observations", "active_tags", "clear_tags", "presence_unknown",
                "failed_calls", "native_identity_unavailable", "unframed", "resource_changes")
        }
        roles_stats[role]["distinct_native_resources"] = len(distinct[role])

    drops = summary.get("dropped_events")
    exhausted = summary.get("capacity_exhausted")
    return {
        "bundle": str(bundle), "rows": rows,
        "capture": {"dropped_events": drops, "capacity_exhausted": exhausted,
                    "truncated_tag_batches": summary.get("truncated_tag_batches"),
                    "sequences_nonmonotonic": bad_sequence},
        "hud_tag_events": recognized_hud, "older_tags_without_role": legacy_unclassified,
        "role_counts": roles_stats,
        "frame_scoped": {
            "frames_with_any_observed_tag": len(frame_masks),
            "frames_with_observed_pair": frames_with_pair,
            "frames_with_partial_observation": len(frame_masks) - frames_with_pair,
        },
        "fg_options": options,
        "event_counts": dict(other_counts),
        "limits": [
            "Global slSetTag has no FrameToken; frame-scoped counts exclude it.",
            "Missing records cannot prove missing game tags, especially if capture dropped events.",
            "Successful slSetTag and matching resource IDs do not validate GPU pixel content or UI stability.",
            "The observer can see different wrapper boundaries depending on hook installation order.",
        ],
    }


def analyze(bundles: list[Path]) -> dict[str, Any]:
    outputs = [load(item) for item in bundles]
    comparison = None
    if len(outputs) == 2:
        left, right = outputs
        comparison = {
            role: {
                field: right["role_counts"][role][field] - left["role_counts"][role][field]
                for field in left["role_counts"][role]
            } for role in HUD_ROLES
        }
    return {"tool": "mfgdiagnostics_hud_offline_r1", "sessions": outputs,
            "comparison_right_minus_left": comparison,
            "diagnosis": "observations_only_not_pixel_or_causal_proof"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundles", nargs="+", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if not 1 <= len(args.bundles) <= 2:
        parser.error("Supply one or two bundle directories")
    report = analyze(args.bundles)
    serialized = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(serialized, encoding="utf-8")
        print(f"REPORT={args.out}")
    else:
        print(serialized, end="")


if __name__ == "__main__":
    main()
