---
name: neverd-initialization-coverage
description: Verify byte initialization and output evidence in NeverD scans of owned native programs. Use for runtime-indexed or backwards reads, partial writes, local stack loads, returned bytes and interpreting indexed_read_output or UNKNOWN.
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

## Check a runtime index and its output

Use a fully initialized input for the native reference run. Record the compiled function, its input extent and its output contract. For a backwards lookup, account for the largest distance, the smallest distance and the whole load width. Use the same compiled function object for the scan and the reference executable.

Read `analyzers.audit.indexed_read_output`. On a single retained indexed load, the nonzero bucket in each histogram answers the corresponding question. For an image with several reads, the counters summarize the inventory. Use a function-sized control to check an individual case.

| Compiled control | Written before read | Reaches function output |
| --- | --- | --- |
| Initialize a word, copy a byte from a bounded runtime distance to the returned cursor | YES | YES, returned storage |
| Return the selected initialized byte in a scalar | YES | YES, returned value |
| Fully replace the only copied byte at an independent returned address before exit | YES | NO |
| Use the selected byte to compute the returned address, then overwrite the copied byte | YES | UNKNOWN |
| The allowed source range extends beyond the known write | UNKNOWN | UNKNOWN |
| Apply arithmetic to an initialized byte before returning it | YES | UNKNOWN for whole-byte transport |
| A recognized native indexed load vanished from machine MedIR | UNKNOWN | UNKNOWN, with `unmatched_native` |

One unresolved indexed read contributes `UNKNOWN` to the scan verdict unless an independent finding establishes `UNSAFE`. Zero indexed reads leave this property's verdict `UNKNOWN`. Verify both the JSON and the CLI exit code. For normal library inputs, compare output bytes or a known return value against a reference even when the static result is inconclusive.

## Interpret the result

Read `analyzers.audit.direct_read_initialization`. Each direct MedIR load or supported read-modify-write visited contributes to exactly one counter. `local_stack` means the local reaching-write check handled the read. Findings give its individual outcome. `outside_or_unresolved` leaves the bytes' prior state open. A nonzero `bounded_index_store` counts a load covered across its full runtime index range. The direct-read inventory counts operations in lifted functions. Catalog call effects have separate checks. An absent object means Audit did not enter this pass.

Apple records CVE-2026-113910 in Compression as a memory initialization issue. The article describes a read inside an output buffer while part of that buffer still held prior contents. [Author's account](https://willenborg.dev/blog/self-copy/) · [Apple security update](https://support.apple.com/en-asia/149227)

For a routine filling a caller's output buffer, identify who initialized the source bytes on every relevant path and which bytes reach the returned output. If the evidence ends at an incoming pointer or a runtime-dependent range, record that boundary alongside the aggregate verdict. A single observed constant store establishes only its covered load. An observed uninitialized read alone does not establish that bytes reach an external consumer.

## Verify a change

Run the direct-read cases in `NeverDSafetyTests` and the native scan adapter test. Compare the integrated CLI, worker and Python SDK reports for the same control. For a hot-path change, use the same input binaries and a Release build for interleaved baseline and candidate measurements.

The report contract, proof implementation and machine-code controls are maintained here. [Scan report](../../../docs/memory-safety-scan.md) · [Static audit](../../../lib/safety/AllocLifetime.cpp) · [Indexed byte proof](../../../lib/safety/IndexedReadOutput.h) · [MedIR tests](../../../unittests/safety/AllocLifetimeTests.cpp) · [Native adapter tests](../../../tools/neverd-worker/tests/memory_safety_scan_test.py)
