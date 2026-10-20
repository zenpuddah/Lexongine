# Execution and integrity model

## Typed graph

Operations declare ordered inputs/outputs, parameter schemas, initialization,
history, timing, numerical policies and sharing eligibility. Preparation resolves
these records, validates sources and admits resource requirements before execution.
The core has no market-data backend or Arrow dependency.

The C++ entry points are in `src/lexongine/`: `registry.hpp`, `configuration.hpp`,
`plan.hpp`, `execution.hpp`, `source.hpp` and `result_store.hpp`. Hosts bind source,
observation and evidence services explicitly. Numerical callbacks receive current
typed inputs, not readers, catalogs or future source vectors.

## Scheduling and sharing

A declaration-ordered topological schedule is frozen before sharing. Full recursive
semantic equality and eligibility determine sharing; hashes only select candidates.
Each physical producer executes at its first logical occurrence. Later aliases and
their observers are reached at their original logical positions. Unique effects
are never deduplicated, and earlier effects remain partial evidence after failure.

Equal values at successive keys remain distinct updates. Own startup can advance
state while yielding Warming; dependency waiting resolves the key without calling
the downstream numerical kernel. Publication validates a complete private bundle
before a nonthrowing commit.

## Time, gaps and provenance

Point and interval extents are distinct. Publication availability includes the
current activation and all required parents. Lag retains the selected parent's
original timing without backdating the new publication.

Strict rejects required source gaps. Skip advances only on actual complete inputs,
preserves original keys and state, and emits explicit gap controls. It does not
turn corrupted present data into a gap or manufacture stale/zero observations.
Count history counts usable publications, not elapsed key distance.

Publication metadata records ordered parent references, including the consuming
input port and producing output port. Replayed parents bind to authority established
by the new run; stored metadata cannot mint authority or expose ancestor payloads.

## Ownership and resource accounting

Published values are immutable. Shared holders charge the underlying allocation
once; metadata has separate accounting. Reservations precede managed allocations.
Retained external handles keep resource accounts and module code pins alive after
run closure. Logging records metadata and does not retain payloads.

These are host-managed bounds, not a hard RSS limit on arbitrary native code or
third-party libraries. Plugins are trusted, same-build native modules, not a sandbox
or stable foreign ABI.

## Cross-run reuse

The native store serves exact full-scope output streams. It validates current source
coverage/order, complete computation identity, codec/build contracts, accepted origin,
payload integrity, startup and parent/timing semantics. Corrupt present data fails
preparation instead of silently becoming a miss.

Scope extension reconstructs state from input history. A saved mean is not an SMA
checkpoint. Capacity bypass is chosen before writing a new entry; write failure is
not capacity bypass. The store uses an exclusive writer lock, immutable files and
explicit cleanup, with no automatic repair or eviction.

## Acceptance

Engine completion, host completion and required evidence are separate facts.
Payloads/manifests, logs and the Pending snapshot close before one atomic no-replace
acceptance-marker publication. Provisional files are ineligible. Ambiguous publication
uses bounded reconciliation; unresolved status is Unconfirmed, not success or a
proven rejected origin.

This provides process-level eligibility, not an all-file transaction or a power-loss
durability guarantee. Optional receipt delivery follows acceptance and has its own
failure status.

The host API (`advance_one`, `finish`, `fail_host`, `abort_host`) uses the same
scheduler as standalone execution. Host trial/account state stays outside pure
market-only features unless explicitly represented as declared inputs.

## External native-host build identity

External applications can bind their own code and configuration to the inspected
native build using the exported CMake helper:

```cmake
lexongine_bind_host_build(my_host
  ROOT "${absolute_host_source_root}"
  INPUTS ${absolute_host_sources_headers_and_build_files}
  DEPENDENCY_FACTS "explicit dependency revisions"
  BUILD_FACTS "explicit compiler and configuration facts")
```

External calls require all four arguments. Inputs must be existing absolute files;
their sorted content hashes and root-relative names join the exported engine ABI,
core-source and dependency/build facts. Helper scripts resolve against their
definition directory, independently of the caller's directory. Existing in-tree
target-only calls remain supported.

The compiled fingerprint and generated sidecar bind the same value. Runtime
inspection still validates the actual executable, sidecar, loaded dependencies
and SDK contract. Applications own their full input inventory and truthful build
facts. [Quadeplus](https://github.com/zenpuddah/Quadeplus) uses this interface for its
daily research backtester; financial accounts and execution remain application-owned.
