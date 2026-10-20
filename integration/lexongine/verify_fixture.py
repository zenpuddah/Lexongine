"""Independent snapshot oracle: stdlib XML/hashlib/Fraction only, no engine imports.

Can be imported by process tests or run against a retained standard fixture:
  python3 verify_fixture.py SNAPSHOT --receipt RECEIPT
"""

from fractions import Fraction
import argparse
import hashlib
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def value(e):
    if e.tag == "record":
        result = {"_type": e.attrib["type"]}
        for f in e:
            check(
                f.tag == "field" and len(f) == 1 and f.attrib["name"] not in result,
                "duplicate/malformed field",
            )
            result[f.attrib["name"]] = value(f[0])
        return result
    if e.tag == "list":
        check(all(i.tag == "item" and len(i) == 1 for i in e), "malformed list")
        return [value(i[0]) for i in e]
    if e.tag == "optional":
        check(
            e.attrib["present"] in ("true", "false") and len(e) == (e.attrib["present"] == "true"),
            "malformed optional",
        )
        return value(e[0]) if len(e) else None
    if e.tag == "variant":
        check(len(e) == 1, "malformed variant")
        return {"kind": e.attrib["kind"], "value": value(e[0])}
    text = e.text or ""
    if e.tag in ("u64", "i64"):
        check(re.fullmatch(r"0|-?[1-9][0-9]*", text) is not None, "noncanonical integer")
        n = int(text)
        check(0 <= n < 2**64 if e.tag == "u64" else -(2**63) <= n < 2**63, "integer range")
        return n
    if e.tag == "bool":
        check(text in ("true", "false"), "invalid boolean")
        return text == "true"
    if e.tag == "digest":
        check(re.fullmatch("[0-9a-f]{64}", text) is not None, "invalid digest")
        return text
    check(e.tag == "text", f"unknown scalar {e.tag}")
    return text


def canonical(e):
    """Independent implementation of the documented field framing, not XML hashing."""

    def n(v):
        return v.to_bytes(8, "big", signed=False)

    def frame(b):
        return n(len(b)) + b

    if e.tag == "record":
        return b"".join(
            (i + 1).to_bytes(2, "big") + frame(canonical(f[0])) for i, f in enumerate(e)
        )
    if e.tag == "list":
        return n(len(e)) + b"".join(frame(canonical(i[0])) for i in e)
    if e.tag == "optional":
        return b"\x01" + canonical(e[0]) if e.attrib["present"] == "true" else b"\x00"
    if e.tag == "variant":
        tag = {
            "signed64": 1,
            "unsigned64": 2,
            "boolean": 3,
            "text": 4,
            "point": 1,
            "interval": 2,
            "count": 1,
        }[e.attrib["kind"]]
        return bytes([tag]) + canonical(e[0])
    if e.tag in ("u64", "i64"):
        return n(int(e.text) % 2**64)
    if e.tag == "bool":
        return b"\x01" if e.text == "true" else b"\x00"
    if e.tag == "digest":
        return bytes.fromhex(e.text)
    check(e.tag == "text", "unknown canonical kind")
    return (e.text or "").encode("utf-8")


def identity_digest(e, kind):
    domain = b"lexongine.identity.v1"
    return hashlib.sha256(
        len(domain).to_bytes(8, "big") + domain + kind.to_bytes(2, "big") + canonical(e)
    ).hexdigest()


def child(root, name):
    matches = root.findall(name)
    check(len(matches) == 1 and len(matches[0]) == 1, f"missing/duplicate section {name}")
    return matches[0][0]


def verify_reference(ref):
    check(ref["finalized"], "unfinalized required evidence")
    path = Path(ref["absolute_path"])
    check(path.is_absolute(), "relative evidence path")
    data = path.read_bytes()
    check(
        len(data) == ref["byte_count"] and hashlib.sha256(data).hexdigest() == ref["digest"],
        f"evidence integrity: {path}",
    )


def verify_marker(path, digest):
    data = path.read_bytes()
    check(hashlib.sha256(data).hexdigest() == digest, "authoritative marker hash")
    marker = ET.fromstring(data)
    check(
        marker.tag == "accepted_run"
        and marker.attrib["version"] == "1"
        and marker.attrib["acceptance"] == "accepted"
        and marker.attrib["numerical"] == "completed"
        and marker.attrib["integration_outcome"] == "completed"
        and marker.attrib["cache_status"] == "published",
        "marker status",
    )
    check(path.name == marker.attrib["commit_id"] + ".accepted.xml", "marker filename identity")
    inventory = value(child(marker, "evidence_inventory"))
    check(
        len({e["name"] for e in inventory}) == len(inventory)
        and len({e["absolute_path"] for e in inventory}) == len(inventory),
        "marker inventory duplicates",
    )
    for ref in inventory:
        check(Path(ref["absolute_path"]) != path, "marker self-reference")
        verify_reference(ref)
    snapshot = value(child(marker, "snapshot_reference"))
    check(
        any(
            e["absolute_path"] == snapshot["absolute_path"]
            and e["digest"] == snapshot["digest"]
            and e["byte_count"] == snapshot["byte_count"]
            for e in inventory
        ),
        "marker snapshot omitted",
    )
    origin = ET.parse(snapshot["absolute_path"]).getroot()
    check(
        origin.attrib["run_id"] == marker.attrib["run_id"]
        and origin.attrib["acceptance"] == "pending",
        "marker/snapshot origin",
    )
    origin_cache = value(child(origin, "cache"))
    entries = value(child(marker, "entries"))
    check(
        origin_cache["commit_id"] == marker.attrib["commit_id"]
        and origin_cache["entries"] == entries,
        "origin entry/commit mismatch",
    )
    for entry in entries:
        base = path.parent / entry["artifact_id"]
        for suffix, key in ((".manifest.xml", "manifest"), (".price.xml", "payload")):
            content = Path(str(base) + suffix).read_bytes()
            check(
                len(content) == entry[key + "_bytes"]
                and hashlib.sha256(content).hexdigest() == entry[key + "_digest"],
                "accepted artifact hash",
            )
        manifest = ET.parse(str(base) + ".manifest.xml").getroot()
        check(
            manifest.attrib["origin_run_id"] == marker.attrib["run_id"]
            and manifest.attrib["artifact_id"] == entry["artifact_id"],
            "manifest association",
        )
        identity = bytes.fromhex(value(child(manifest, "canonical_identity")))
        check(
            hashlib.sha256(identity).hexdigest()
            == entry["identity_digest"]
            == manifest.attrib["identity_digest"],
            "complete artifact identity hash",
        )
        check(
            manifest.attrib["payload_digest"] == entry["payload_digest"]
            and int(manifest.attrib["payload_bytes"]) == entry["payload_bytes"],
            "manifest/payload association",
        )
        coverage_element = child(manifest, "coverage")
        check(
            identity_digest(coverage_element, 4) == manifest.attrib["coverage_digest"],
            "coverage framing digest",
        )
        coverage = value(coverage_element)
        payload = ET.parse(str(base) + ".price.xml").getroot()
        check(
            [int(o.attrib["step"]) for o in payload]
            == [k["sequence"] for k in coverage["complete_keys"]],
            "payload complete-key coverage",
        )
    return marker, inventory, entries


def verify_acceptance(snapshot_path, root, receipt):
    check(
        receipt["acceptance"] == "accepted"
        and receipt["numerical"] == "completed"
        and receipt["integration"] == "completed",
        "receipt not accepted",
    )
    check(root.attrib["acceptance"] == "pending", "snapshot independently claims acceptance")
    evidence = receipt["evidence"]
    check(len({e["name"] for e in evidence}) == len(evidence), "duplicate evidence")
    for ref in evidence:
        verify_reference(ref)
    snapshots = [e for e in evidence if Path(e["absolute_path"]) == snapshot_path]
    check(len(snapshots) == 1, "receipt omits snapshot")
    embedded = value(child(root, "precommit_evidence_inventory"))
    check(
        embedded == [e for e in evidence if e not in snapshots],
        "self-referencing/extra snapshot inventory",
    )
    resources = value(child(root, "resources"))
    check(
        resources["required_evidence"] == resources["plan"]["required_evidence"],
        "evidence policy changed",
    )
    check(
        {s["name"] for s in resources["required_evidence"]} == {e["name"] for e in evidence},
        "required evidence omitted",
    )
    config = resources["plan"]["effective_composition"]
    if receipt["cache"] == "published":
        marker_path = Path(config["cache"]["directory"]) / (receipt["commit_id"] + ".accepted.xml")
        marker, inventory, entries = verify_marker(marker_path, receipt["record_digest"])
        check(
            marker.tag == "accepted_run"
            and marker.attrib["acceptance"] == "accepted"
            and marker.attrib["run_id"] == receipt["run_id"],
            "marker origin/status",
        )
        check(inventory == evidence, "marker inventory differs")
        check(entries == value(child(root, "cache"))["entries"], "staged/accepted entries differ")
    else:
        check(
            not receipt["commit_id"] and receipt["published_entries"] == 0,
            "no-entry run published a marker",
        )
    checked = set()
    for origin in value(child(root, "cache"))["origins"]:
        if origin["commit_id"] in checked:
            continue
        checked.add(origin["commit_id"])
        marker, _, _ = verify_marker(
            Path(config["cache"]["directory"]) / (origin["commit_id"] + ".accepted.xml"),
            origin["record_digest"],
        )
        check(marker.attrib["run_id"] == origin["run_id"], "reused origin run mismatch")


def verify_snapshot(
    snapshot, receipt=None, inputs=None, interval_ns=None, expected_parameters=None
):
    root = ET.parse(snapshot).getroot() if not isinstance(snapshot, ET.Element) else snapshot
    check(root.tag == "run_snapshot" and root.attrib["version"] == "1", "snapshot envelope")
    check(
        root.attrib["numerical"] == "completed"
        and root.attrib["integration_outcome"] == "completed",
        "incomplete run",
    )
    resources_element = child(root, "resources")
    plan_element = resources_element.find("field[@name='plan']/record")
    check(
        plan_element is not None and identity_digest(plan_element, 3) == root.attrib["plan_digest"],
        "plan digest differs from independent framing",
    )
    resources = value(resources_element)
    plan, counters = resources["plan"], value(child(root, "counters"))
    config = plan["effective_composition"]
    nodes = config["nodes"]
    names = {n["spec"]["id"]: i for i, n in enumerate(nodes)}
    aliases = {a["logical"]: a["representative"] for a in plan["aliases"]}
    order = plan["logical_order"]
    check(sorted(order) == list(range(len(nodes))), "logical schedule permutation")
    source = config["sources"][0]
    coverage = source["coverage"]
    keys = [k["sequence"] for k in coverage["complete_keys"]]
    scope = coverage["expected_scope"]
    if inputs is None:
        check(
            source["snapshot_name"] == "fixture-bars-v1",
            "explicit independent inputs required for nonstandard source",
        )
        inputs = {k: 100000000000 + 10000000 * k for k in range(120)}
        interval_ns = 60000000000
    check(interval_ns is not None, "explicit timing required")
    check(
        keys == sorted(k for k in inputs if scope["begin"] <= k < scope["end"]),
        "source coverage differs from independent inputs",
    )
    covered = set(keys)
    check(
        not coverage["gaps"]
        or all(
            n["spec"]["inputs"][0]["missing_intervals"] == "skip"
            for n in nodes[1:]
            if n["spec"]["inputs"]
        ),
        "Strict accepted source gaps",
    )
    for gap in coverage["gaps"]:
        missing = set(range(gap["missing_scope"]["begin"], gap["missing_scope"]["end"]))
        check(not covered.intersection(missing), "coverage overlap")
        covered.update(missing)
    check(covered == set(range(scope["begin"], scope["end"])), "unidentified coverage hole")
    params = [
        {p["name"]: p["value"]["value"] for p in n["spec"]["parameters"]["values"]} for n in nodes
    ]
    if expected_parameters:
        for name, expected in expected_parameters.items():
            check(
                all(params[names[name]].get(k) == v for k, v in expected.items()),
                f"ignored configured parameters for {name}",
            )
    digests = [hashlib.sha256(bytes.fromhex(n["canonical_identity"])).hexdigest() for n in nodes]
    source_ref = source["reference_digest"]
    history = [[] for _ in nodes]
    expected, sequence, logical_values = {}, [], {}
    invocations = [0] * len(nodes)
    publications = [0] * len(nodes)
    clocks = scope["clock"]

    def parent(input_port, producer, key):
        return {
            "input_port": input_port,
            "publication": {
                "computation_digest": digests[producer],
                "key": {"clock": clocks, "sequence": key},
                "port": 0,
                "extent": {
                    "kind": "interval",
                    "value": {"begin_ns": key * interval_ns, "end_ns": (key + 1) * interval_ns},
                },
                "available_at_ns": (key + 1) * interval_ns,
                "source_reference_digest": source_ref if producer == 0 else None,
            },
        }

    for position, key in enumerate(keys):
        for i in order:
            node, spec = nodes[i], nodes[i]["spec"]
            if not node["needed"]:
                continue
            rep = aliases[i]
            operation = spec["operation"]["name"]
            incoming = [names[v["producer"]] for v in spec["inputs"]]
            previous = [expected[j, key] for j in incoming]
            outcome, raw, parents, remaining = "published", None, [], 0
            if i == 0:
                raw = inputs[key]
                invocations[i] += 1
            elif any(v["kind"] == "warming" for v in previous):
                outcome = "warming"
                remaining = max(0, node["preceding"] + 1 - (position + 1))
            else:
                invocations[i] += 1
                if operation == "bar_close":
                    raw = previous[0]["raw"]
                    parents = [parent(0, incoming[0], key)]
                elif operation == "sma_price":
                    window = params[i]["window"]
                    bound = params[i]["max_abs_input_raw"]
                    check(
                        window > 0
                        and 0 <= bound <= (2**63 - 1) // window
                        and abs(previous[0]["raw"]) <= bound,
                        "SMA numeric domain",
                    )
                    check(
                        (params[i]["sum_width"], params[i]["rounding"], params[i]["overflow"])
                        == ("signed64", "nearest_ties_even", "fail"),
                        "SMA numeric policy",
                    )
                    samples = history[i] + [(key, previous[0]["raw"])]
                    history[i] = samples
                    if len(samples) < window:
                        outcome, remaining = "warming", window - len(samples)
                        parents = [parent(0, incoming[0], key)]
                    else:
                        selected = samples[-window:]
                        raw = round(Fraction(sum(v for _, v in selected), window))
                        parents = [parent(0, incoming[0], k) for k, _ in selected]
                elif operation == "lag_price":
                    distance = params[i]["lag_steps"]
                    check(
                        distance > 0 and params[i]["count_basis"] == "complete_bar_updates",
                        "lag basis",
                    )
                    if len(history[i]) < distance:
                        outcome, remaining = "warming", distance - len(history[i])
                        parents = [parent(0, incoming[0], key)]
                    else:
                        selected_key, raw = history[i][-distance]
                        parents = [parent(0, incoming[0], selected_key)]
                    history[i].append((key, previous[0]["raw"]))
                elif operation == "price_difference":
                    raw = previous[0]["raw"] - previous[1]["raw"]
                    parents = [
                        parent(port, producer, key) for port, producer in enumerate(incoming)
                    ]
                else:
                    raise AssertionError(f"oracle has no semantics for {operation}")
            publications[i] += outcome == "published"
            expected[i, key] = {
                "kind": outcome,
                "raw": raw,
                "parents": parents,
                "remaining": remaining,
            }
            if rep == i:
                sequence.append((key, i))
            else:
                check(
                    digests[i] == digests[rep] and expected[i, key] == expected[rep, key],
                    "alias is not semantically equivalent",
                )
    events = value(child(root, "outcomes"))
    actual_sequence, gap_events, actual_timeline = [], [], []

    def semantic(v):
        if isinstance(v, dict):
            return {k: semantic(x) for k, x in v.items() if k != "_type"}
        if isinstance(v, list):
            return [semantic(x) for x in v]
        return v

    for event in events:
        if event["kind"] == "gap":
            gap_events.append(event["gap"])
            actual_timeline.append(("gap", event["gap"]["missing_scope"]["begin"]))
            continue
        actual = event["outcome"]
        i, key = actual["logical"], actual["key"]["sequence"]
        actual_sequence.append((key, i))
        actual_timeline.append(("outcome", key, i))
        wanted = expected[i, key]
        check(
            actual["kind"] == wanted["kind"]
            and actual["remaining_preceding"] == wanted["remaining"],
            f"outcome/readiness {i}/{key}",
        )
        check(
            actual["key"]["clock"] == clocks
            and actual["available_at_ns"] == (key + 1) * interval_ns,
            f"causal timing {i}/{key}",
        )
        check(
            semantic(actual["extent"])
            == {
                "kind": "interval",
                "value": {"begin_ns": key * interval_ns, "end_ns": (key + 1) * interval_ns},
            },
            "extent changed",
        )
        check(semantic(actual["parents"]) == wanted["parents"], f"ordered parent lineage {i}/{key}")
        if wanted["kind"] == "published":
            check(len(actual["values"]) == 1, "incomplete bundle")
            v = actual["values"][0]
            check(v["port"] == 0, "output port changed")
            if i:
                meaning = {p["name"]: p["value"]["value"] for p in v["type"]["meaning"]["values"]}
                check(
                    v["type"]["nominal_name"] == "qaedalus.price"
                    and v["type"]["semantic_version"] == 1
                    and meaning["scale"] == 9,
                    "output interpretation",
                )
                check(
                    v["encoding"] == "price9"
                    and v["raw"] == wanted["raw"]
                    and -(2**63) <= v["raw"] < 2**63,
                    f"numeric value {i}/{key}: {v['raw']} != {wanted['raw']}",
                )
            else:
                check(
                    v["encoding"] == "source_reference" and v["source_reference"] == source_ref,
                    "source association changed",
                )
        else:
            check(not actual["values"], "warming fabricated a value")
    check(actual_sequence == sequence, "physical outcomes missing/duplicated/reordered")
    check(gap_events == coverage["gaps"], "gap evidence missing/duplicated")
    gap_starts = {g["missing_scope"]["begin"] for g in coverage["gaps"]}
    expected_timeline = []
    for key in sorted(set(keys) | gap_starts):
        expected_timeline.extend(
            [("gap", key)]
            if key in gap_starts
            else [("outcome", k, i) for k, i in sequence if k == key]
        )
    check(actual_timeline == expected_timeline, "gap/outcome timeline order")
    check(
        counters["complete_updates"] == len(keys) and counters["gap_events"] == len(gap_events),
        "timeline counters",
    )
    check(counters["missing_intervals"] == len(covered) - len(keys), "missing interval count")
    for i, p in enumerate(counters["producers"]):
        check(p["id"] == nodes[i]["spec"]["id"], "counter attribution")
        if aliases[i] != i:
            check(p["advances"] == p["cached_deliveries"] == 0, "alias executed")
            check(p["aliases"] == len(keys) and p["publications"] == 0, "alias occurrence count")
        elif nodes[i]["needed"]:
            check(
                (p["advances"], p["cached_deliveries"]) in ((invocations[i], 0), (0, len(keys))),
                "advance/delivery counters",
            )
            check(p["publications"] == publications[i], "publication counter")
    for request in value(child(root, "requests")):
        r = request["request"]
        producer = names[r["producer"]]
        selected = [
            (k, expected[producer, k]["raw"])
            for k in keys
            if r["evaluation"]["begin"] <= k < r["evaluation"]["end"]
        ]
        check(request["admitted"] and request["delivered"] == len(selected), "request mask/count")
        check(
            all(expected[producer, k]["kind"] == "published" for k, _ in selected),
            "request accepted warming",
        )
        logical_values[r["id"]] = selected
    observations = [
        e for e in value(child(root, "precommit_evidence_inventory")) if e["name"] == "observations"
    ]
    if observations:
        check(len(observations) == 1, "duplicate observer trace")
        verify_reference(observations[0])
        lines = Path(observations[0]["absolute_path"]).read_text().splitlines()
        check(lines[0] == "lexongine-observations-v1", "observer trace format")
        actual = [
            (parts[0], *map(int, parts[1:])) for parts in (line.split("\t") for line in lines[1:])
        ]
        wanted = [
            (r["id"], k, expected[i, k]["raw"], (k + 1) * interval_ns)
            for k in keys
            for i in order
            for r in config["observations"]
            if names[r["producer"]] == i and r["evaluation"]["begin"] <= k < r["evaluation"]["end"]
        ]
        check(
            actual == wanted,
            "actual logical observer occurrences/order differ from independent masks",
        )
    result = {
        "requests": logical_values,
        "outcomes": {(nodes[i]["spec"]["id"], k): semantic(v) for (i, k), v in expected.items()},
        "source_digest": source["content_digest"],
        "plan_digest": root.attrib["plan_digest"],
    }
    if receipt is not None:
        decoded = (
            value(ET.parse(receipt).getroot()[0])
            if isinstance(receipt, (str, Path))
            else value(receipt[0])
        )
        verify_acceptance(Path(snapshot).resolve(), root, decoded)
        check(decoded["complete_updates"] == len(keys), "receipt/snapshot counters")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    result = verify_snapshot(args.snapshot, args.receipt)
    print(
        "PASS independent exported-run oracle", {k: len(v) for k, v in result["requests"].items()}
    )


if __name__ == "__main__":
    main()
