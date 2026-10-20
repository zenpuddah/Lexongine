# Verification

## Formatting and lint update — 2026-10-20

The formatted public tree passes the pinned [style checks](contributing.md),
including Python lint and a no-change formatter check. A fresh macOS arm64 Debug
standalone build passes **24/24 tests**. The four optional reader/integration tests
also pass against the formatted public Quadeplus tree, using Arrow/Parquet 26.0.0.
Canonical headers and the file-integrity helper remain byte-identical in both repos.

All 104 existing C++ files reproduce exactly from the pinned formatter applied to
the preceding release source, and CMake token sequences are unchanged. Python AST
comparison accounts for two local lambda-to-function lint cleanups in the independent
checker; other Python syntax trees are unchanged. Two documented import-order lint
exceptions preserve bytecode suppression before local verification imports.
GitHub Actions checks formatting and Python lint; native runtime verification is
the separate macOS profile described here.

## Initial v1.0.0 test profiles

The standalone v1 release tree passed **24/24 tests** in both normal and ASan/UBSan
builds on macOS arm64. The external Qaedalus profile passed **28/28 tests** in a
normal build against the compatible checkout. These are separate configurations;
the standalone profile does not require the external market-data implementation.

The default standalone profile exercises the core, numerical components, XML,
CLI, native plugins/store, hosted probes and independent Python oracles. The
optional Qaedalus profile additionally compiles the real checked reader interface
and persisted-acquisition integration against an explicit external checkout.

```sh
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/sanitized --output-on-failure
```

Both profiles require native macOS checks for the plugin/filesystem guarantees.
Disabling the external bridge is not evidence that its integration tests passed.

## Coverage

- Exact sharing, forced hash collisions and ordered transitive identity.
- Immutable ownership, bounded history, causal reads and atomic bundles.
- Strict/Skip gaps, original keys, startup and state continuity.
- Unique-effect ordering and independent hosted trial occurrences.
- Full-scope reuse, source validation before hits, corruption and capacity gates.
- Required evidence failure, uncertain publication and abrupt process exit/restart.
- CLI overrides, strict XML, escaped DOT, immutable outputs and stable exit codes.
- Original 120-Bar fixture and lag variant in fresh, cached and unshared modes.
- Every hosted split position of the original fixture and every split of the
  checked three-Trade adapter probe.

The checked replay tests preserve original acquisition succession even when event
timestamps repeat or regress. Filtered ordinal gaps are not automatically source gaps.

## Independent exported-run oracle

The Python checker uses standard-library XML/hashlib and exact `Fraction`; it does
not call production numerical helpers. It recomputes logical outcomes independently
of alias declarations and checks values, readiness, parents, availability, masks,
actual observer traces and evidence/artifact hash linkage.

```sh
python3 integration/lexongine/verify_fixture.py /absolute/run.snapshot.xml \
  --receipt /absolute/run.receipt.xml
```

The deterministic acceptance harness uses seed `20261018` and covers repeated,
signed, gapped and random data, exact-scope extension and deliberate corruption.
To retain its output, choose a new directory:

```sh
python3 integration/lexongine/verify_acceptance.py \
  build/debug/bin/lexongine_acceptance_probe build/debug \
  --retain build/debug/acceptance-run
```

The harness deliberately corrupts test-store payloads at the end to verify rejection.
Its retained stores are negative-test artifacts, not reusable application data.

## Measurements and limits

The harness records source/config/plan/build identities, numerical counters and
phase/resource measurements. Preparation, instantiation, advancement, finalization
and receipt encoding are separate phases. Retention, snapshot and marker-publication
times are overlapping subtimers. Summed account/category peaks are upper bounds,
not simultaneous RSS.

Native XML result validation can cost more than recalculation on tiny workloads.
Zero cached numerical advances therefore do not imply lower end-to-end latency.
The current implementation favors traceable behavior over throughput claims.

Validated native environment: C++20 AppleClang 21/libc++, TinyXML2 11.0.0 and Python
3.14. Optional integration uses Arrow/Parquet 26.0.0. Prebuilt dependencies are not
instrumented merely by enabling project sanitizers. Power loss, arbitrary native
UB, untrusted plugin isolation and financial-model correctness are outside this
verification scope.
