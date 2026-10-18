# Lexongine

A C++20 computation engine for fixed, typed graphs. Lexongine evaluates each
producer once per source update, shares equivalent computations, and can replay
validated outputs across runs.

**Version 1.0.0 · macOS arm64**

## What it does

- Typed inputs and immutable output bundles, with causal availability checks.
- Explicit initialization, history, missing-data and numerical policies.
- Deterministic logical scheduling that preserves observation and effect order.
- Exact computation sharing and bounded, full-scope result reuse.
- Resource accounting, provenance and required execution evidence.
- XML configuration, a CLI, graph exports and trusted same-build native plugins.

The reference components use fixed-point prices: Bar close, signed-integer SMA,
fixed-count lag and ordered subtraction. The included fixture generates 120
synthetic Bars; it does not acquire market data or implement trading simulation.

## Build

Requires CMake 3.25+, an Apple C++20 toolchain, Python 3 for the verification tools,
and **TinyXML2 11.0.0** supplied explicitly.

```sh
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEXONGINE_TINYXML2_SOURCE_DIR=/absolute/path/to/tinyxml2-11.0.0
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure
```

No dependencies are downloaded by this build. See [build instructions](docs/build.md)
for package-based TinyXML2, sanitizer builds, CLion and the optional Qaedalus adapter.

## Run the fixture

```sh
build/debug/bin/lexongine list
build/debug/bin/lexongine describe \
  --operation statistics.sma_price --version 1 --implementation builtin-v1

out=$(mktemp -d /tmp/lexongine.XXXXXX)
build/debug/bin/lexongine run \
  --config docs/lexongine/examples/cli-computation.xml \
  --reuse disabled \
  --report "$out/run.snapshot.xml" \
  --retention-log "$out/retention.log"
```

The fixture has six logical nodes and five shared physical producers. Four
observations receive 71 values each; the fast-minus-slow spread is `150000000`
in raw Price9 units (`0.15`).

See [CLI usage](docs/usage.md) for validation, plan/DOT exports, overrides, result
reuse and exit codes.

## Design and verification

Lexongine separates numerical results from run acceptance. A run is accepted only
after its participating host and required evidence complete. New cache entries
become eligible through one authoritative acceptance marker; a snapshot alone is
not proof of acceptance.

Tests cover shared/unshared/cached equivalence, signed rounding, gaps, ownership,
causal reads, source validation, unique effects, host failures and restart behavior.
A separate Python `Fraction` oracle checks exported values, timing and lineage.
See [design](docs/design.md) and [verification](docs/verification.md).

## Scope

v1 is synchronous and supports one selected source/clock with count-based history.
Native plugins and the filesystem result store target trusted same-build macOS
processes. Reuse serves complete output streams; it does not restore reducer state
or splice partial scopes. Other platforms are not verified.

The standalone build includes the engine, reference components and the small set
of canonical types/checksum helpers they need. The broader Qaedalus market-data
reader integration is optional and uses an external checkout. Orders, fills,
portfolio accounting and live execution are outside this repository's scope.
