---
name: neverd-initialization-coverage
description: Check whether NeverD scan proves that bytes in an owned native program were initialized before a read. Use for local stack reads, caller-provided buffers, partial writes, and interpreting direct_read_initialization when evaluating a coverage claim.
---

# Verify read initialization coverage

Use the native static audit to answer a byte-level question. Identify the source address, the number of bytes consumed, and which writes can reach that read. A bounds check establishes where those bytes are. Initialization requires evidence of their prior contents.

## Establish an observable control

1. Pick a small program whose source and storage contract you know. Record the compiler target, flags, binary hash and NeverD revision. Use a returned value or another ordinary observable use to keep the load in the compiled program.
2. Run `neverd scan <binary> --analyzer=audit`. Capture the JSON and exit status. Check the format, architecture, Audit state, `coverage_complete` and any `uninitialized_read` findings.
3. Compare programs that differ in one property. The repository has Win64 controls for the following cases. Each has one direct read and finishes with aggregate `SAFE`.

| Ordinary operation on a caller field | Expected Audit evidence |
| --- | --- |
| Read a word with unknown incoming contents | One `outside_or_unresolved` |
| Store a constant word, then immediately read its first byte | One `preceding_constant_store` |
| Store one byte, then read the whole word | One `outside_or_unresolved` |

For local stack storage, compare a covered load with a read before the write. Inspect the finding's instruction address and verdict. A read spanning the entry stack pointer also depends on caller storage. When the compiler removes or reorders the operations, inspect the machine MedIR consumed by Audit before changing an expectation.

## Interpret the result

Read `analyzers.audit.direct_read_initialization`. Each direct MedIR load or supported read-modify-write visited contributes to exactly one counter. `local_stack` means the local reaching-write check handled the read. Findings give its individual outcome. `outside_or_unresolved` leaves the bytes' prior state open. The counters omit reads inside called functions. An absent object means Audit did not enter this pass.

For a routine filling a caller's output buffer, identify who initialized the source bytes on every relevant path and which bytes reach the returned output. If the evidence ends at an incoming pointer or a runtime-dependent range, record that boundary alongside the aggregate verdict. A single observed constant store establishes only its covered load. An observed uninitialized read alone does not establish that bytes reach an external consumer.

## Verify a change

Run the direct-read cases in `NeverDSafetyTests` and the native scan adapter test. Compare the integrated CLI, worker and Python SDK reports for the same control. For a hot-path change, use the same input binaries and a Release build for interleaved baseline and candidate measurements.

The report contract, proof implementation and machine-code controls are maintained here. [Scan report](../../../docs/memory-safety-scan.md) · [Static audit](../../../lib/safety/AllocLifetime.cpp) · [MedIR tests](../../../unittests/safety/AllocLifetimeTests.cpp) · [Native adapter tests](../../../tools/neverd-worker/tests/memory_safety_scan_test.py)
