# CLI usage

Run commands from the repository root. Examples use `build/debug/bin/lexongine`.

## Inspect operations and configurations

```sh
build/debug/bin/lexongine list
build/debug/bin/lexongine describe --operation statistics.sma_price --version 1 --implementation builtin-v1
build/debug/bin/lexongine validate --config docs/lexongine/examples/cli-computation.xml --reuse disabled
```

`list` and `describe` emit XML contracts, parameter constraints, types, policies and
build facts. Composition commands load only explicitly configured plugins before
freezing the registry. `validate` performs preparation and source/candidate checks
without executing numerical kernels or opening run evidence.

## Export a plan

```sh
out=$(mktemp -d /tmp/lexongine-plan.XXXXXX)
build/debug/bin/lexongine plan \
  --config docs/lexongine/examples/cli-computation.xml \
  --reuse disabled \
  --plan "$out/plan.xml" \
  --before-dot "$out/logical.dot" \
  --after-dot "$out/physical.dot"
```

DOT is a visualization, not the execution schedule. The physical view annotates
aliases while preserving distinct observation requests. Graphviz is optional.

## Run and reuse results

The example config declares an explicit disposable cache path. Choose an appropriate
absolute directory in the XML before using a long-lived store. All report/log/export
paths are explicit, absolute and create-new.

```sh
out=$(mktemp -d /tmp/lexongine-run.XXXXXX)
build/debug/bin/lexongine run \
  --config docs/lexongine/examples/cli-computation.xml \
  --report "$out/first.snapshot.xml" --retention-log "$out/first.log" \
  > "$out/first.receipt.xml"
build/debug/bin/lexongine run \
  --config docs/lexongine/examples/cli-computation.xml \
  --report "$out/repeat.snapshot.xml" --retention-log "$out/repeat.log" \
  > "$out/repeat.receipt.xml"
```

With matching source/build/computation identity, the repeat replays four numerical
streams. The source still advances 120 times; cached SMA streams have 120 deliveries
and zero numerical advances. Full source validation remains mandatory.

The required `run_snapshot` is finalized with **Pending** acceptance before marker
publication. The terminal `run_receipt` reports final engine, host, acceptance and
cache outcomes. When new entries exist, the store marker is the sole persisted
acceptance authority. Disabled/no-new-entry runs accept in memory after their
declared evidence closes and create no hidden marker.

## Overrides

```sh
build/debug/bin/lexongine validate \
  --config docs/lexongine/examples/cli-computation.xml \
  --set fast_A.window=21 --observe fast_A_trial --reuse disabled
```

Repeated `--set producer.PARAM=VALUE` and `--observe REQUEST_ID` are supported.
Unknown/duplicate targets and invalid typed values reject. Overrides are applied
before full preparation and are recorded in the effective plan.

- `--sharing exact|disabled`
- `--reuse automatic_read_write|disabled`
- `--retention-log /absolute/path`

Enabling reuse requires a complete cache declaration; it cannot invent missing
policies or paths. The graph and parameters cannot change during a run.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Successful inspection or accepted run; all selected requests admitted |
| 2 | Syntax/configuration/preparation rejection |
| 3 | Execution/required evidence failure, or optional stdout delivery failure |
| 4 | API/internal/build/platform prerequisite failure |
| 5 | Acceptance remains Unconfirmed |
| 6 | Accepted run with explicitly reported preparation-rejected requests |

An optional stdout failure does not revoke confirmed acceptance or rewrite evidence.
The receipt and authoritative marker distinguish acceptance from delivery.

## Resource limits

`cli-computation.xml` declares the native fixture's conservative metadata/evidence
budgets, including a 512-MiB managed ceiling. The CLI does not increase document
limits automatically. These accounting ceilings are not process-RSS guarantees or
minimum-memory requirements. The smaller `first-computation.xml` retains the core
fixture configuration used by tests.
