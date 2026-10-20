"""Reproducible exported-run acceptance and measurements; no production math imported."""

import argparse
import copy
import csv
import json
import hashlib
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
from verify_fixture import (  # noqa: E402 -- disable bytecode before local imports
    check,
    value,
    verify_snapshot,
    verify_marker,
    verify_reference,
)

SEED = 20261018
METRICS = [
    "prepare_ns",
    "instantiate_ns",
    "advance_ns",
    "finalize_ns",
    "retention_sub_ns",
    "snapshot_sub_ns",
    "publish_sub_ns",
    "receipt_ns",
    "runtime_peak_upper_bytes",
    "preparation_lifetime_peak_upper_bytes",
    "store_lifetime_peak_upper_bytes",
    "evidence_lifetime_peak_upper_bytes",
    "source_digest",
    "plan_digest",
    "binary_digest",
]


def exercise(probe, output):
    random_source = random.Random(SEED)
    cases = {
        "repeated": (3, 1, "strict", [2, 2, 2, 8, 2, 2]),
        "extension": (2, 1, "strict", [1, 1, 3, 5]),
        "ties": (2, 1, "strict", [1, 0, -1, 0, 3, 0, -3, 0]),
        "skip": (2, 1, "skip", [100, None, 110, 120]),
        "leading_tail": (2, 1, "skip", [None, 5, 5, 7, None]),
        "strict_gap": (2, 1, "strict", [100, None, 110, 120]),
        "all_gap": (2, 1, "skip", [None] * 4),
    }
    for index in range(6):
        values = [random_source.randint(-100, 100) for _ in range(12)]
        if index % 2:
            values[1] = None
            values[6] = None
        cases[f"random{index}"] = (
            2 + index % 3,
            1 + index % 2,
            "skip" if index % 2 else "strict",
            values,
        )
    cases_file = output / "cases.txt"
    cases_file.write_text(
        "".join(
            f"{name} {len(v)} {w} {lag} {policy}\n"
            + " ".join("gap" if x is None else str(x) for x in v)
            + "\n"
            for name, (w, lag, policy, v) in cases.items()
        )
    )
    run = subprocess.run(
        [str(probe), str(cases_file), str(output)], text=True, capture_output=True, timeout=600
    )
    check(run.returncode == 0, run.stderr + run.stdout[-2000:])
    (output / "measurements.tsv").write_text(run.stdout)
    compared, summaries, baselines = 0, [], {}
    for row in csv.reader(run.stdout.splitlines(), delimiter="\t"):
        name, mode, status, snapshot, receipt = row[:5]
        window, lag, policy, inputs = cases[name]
        if status == "rejected":
            diagnostic = value(ET.parse(output / receipt).getroot())
            expected = (
                "SourceCoverage"
                if name == "strict_gap"
                else "HistoryInsufficient"
                if name == "all_gap"
                else "SourceInvalid"
                if mode == "invalid_source"
                else "CacheCorrupt"
            )
            check(
                diagnostic["code"] == expected, f"rejection mismatch: {name}/{mode}: {diagnostic}"
            )
            continue
        check(len(row) == 20, "measurement column mismatch")
        expected_window = window + (mode == "changed_window")
        # Artifact corruption is deliberately injected at the end of each batch.
        # Numerical/lineage verification remains independent of that later fault;
        # origin integrity was checked before corruption by the native validator.
        result = verify_snapshot(
            output / snapshot,
            inputs={k: v for k, v in enumerate(inputs) if v is not None},
            interval_ns=10,
            expected_parameters={
                "a": {"window": expected_window},
                "b": {"window": expected_window},
                "lag": {"lag_steps": lag},
            },
        )
        terminal = value(ET.parse(output / receipt).getroot()[0])
        check(terminal["acceptance"] == "accepted", "terminal rejection")
        for ref in terminal["evidence"]:
            verify_reference(ref)
        if mode == "fresh":
            baselines[name] = result
        elif mode in ("cached", "shared", "unshared"):
            check(
                result["outcomes"] == baselines[name]["outcomes"]
                and result["requests"] == baselines[name]["requests"],
                f"logical equivalence {name}/{mode}",
            )
        if mode == "cached":
            check(
                all(p["advances"] == 0 for p in terminal["producers"][1:]),
                "cached producer advanced",
            )
        if mode == "changed_window":
            check(
                result["source_digest"] == baselines[name]["source_digest"],
                "changing parameters changed source data",
            )
            check(
                next(p for p in terminal["producers"] if p["id"] == "a")["advances"] > 0,
                "changed parameters reused incompatible mean",
            )
        if mode == "extended":
            check(
                result["source_digest"] == baselines[name]["source_digest"],
                "scope extension changed fixed snapshot identity",
            )
            check(
                result["requests"]["A"] == [(1, 1), (2, 2), (3, 4)],
                "extended scope failed to reconstruct exact history",
            )
            check(
                next(p for p in terminal["producers"] if p["id"] == "a")["advances"] == 4,
                "scope extension resumed saved reducer state",
            )
        compared += len(result["outcomes"])
        metrics = {
            key: (int(text) if key.endswith(("_ns", "_bytes")) else text)
            for key, text in zip(METRICS, row[5:])
        }
        config_digest = hashlib.sha256(
            (output / f"{name}-{mode}.config.xml").read_bytes()
        ).hexdigest()
        summaries.append(
            {
                "case": name,
                "mode": mode,
                **metrics,
                "config_digest": config_digest,
                "producers": terminal["producers"],
            }
        )
    # Self-check the oracle against independent mutations, not merely good outputs.
    original = ET.parse(output / "repeated-fresh.snapshot.xml").getroot()
    mutations = []
    changed = copy.deepcopy(original)
    raw = changed.find(".//record[@type='SnapshotValueV1']/field[@name='raw']/optional/i64")
    check(raw is not None, "mutation target absent")
    raw.text = str(int(raw.text) + 1)
    mutations.append(changed)
    changed = copy.deepcopy(original)
    outcomes = changed.find("outcomes/list")
    outcomes.remove(outcomes[-1])
    mutations.append(changed)
    changed = copy.deepcopy(original)
    parent = changed.find(".//record[@type='ParentPublication']/field[@name='available_at_ns']/i64")
    parent.text = str(int(parent.text) - 1)
    mutations.append(changed)
    for mutated in mutations:
        try:
            verify_snapshot(mutated, inputs=dict(enumerate(cases["repeated"][3])), interval_ns=10)
        except AssertionError:
            pass
        else:
            raise AssertionError("oracle accepted a deliberate value/coverage/lineage mutation")
    for name in baselines:
        terminal = value(ET.parse(output / f"{name}-fresh.receipt.xml").getroot()[0])
        try:
            verify_marker(
                output / f"{name}.store" / (terminal["commit_id"] + ".accepted.xml"),
                terminal["record_digest"],
            )
        except AssertionError as error:
            check(str(error) == "accepted artifact hash", f"unexpected corruption cause: {error}")
        else:
            raise AssertionError("independent checker accepted intentionally corrupted payload")
    report = {
        "format": "lexongine-acceptance-measurements-v1",
        "seed": SEED,
        "datasets": len(cases),
        "compared_logical_outcomes": compared,
        "timing_note": "retention/snapshot/publication are overlapping subtimers; peaks are sums of category/account maxima, not RSS",
        "runs": summaries,
    }
    (output / "measurements.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        f"PASS independent exported-run acceptance: seed={SEED} datasets={len(cases)} outcomes={compared} mutation_checks={len(mutations)}"
    )
    print("MEASUREMENTS", output / "measurements.json")
    for row in summaries:
        if row["case"] == "random0" and row["mode"] in ("fresh", "cached", "shared", "unshared"):
            print(
                "MEASURE",
                row["mode"],
                {k: row[k] for k in METRICS[:12]},
                "numeric_advances=",
                sum(p["advances"] for p in row["producers"][1:]),
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--retain", type=Path, help="explicit new directory for retained outputs")
    args = parser.parse_args()
    if args.retain:
        args.retain.mkdir()
        exercise(args.probe.resolve(), args.retain.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="acceptance.", dir=args.scratch) as root:
            exercise(args.probe.resolve(), Path(root))


if __name__ == "__main__":
    main()
