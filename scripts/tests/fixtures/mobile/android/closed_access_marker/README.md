# Closed generic access constructor with an unused anonymous marker

Java 8 independently generates a package synthetic access constructor and an
empty constructorless anonymous marker when a nested builder calls its owner's
private constructor. The selected private contract has one closed Set<Integer>
and two nominal inputs. The exact two-instruction wrapper forwards the original
receiver and every physical argument word, dropping only its last marker.

Both source and SDK profiles compile every generated unit and account for all
original definitions and helper roles. The report must disclose the marker's
named source mapping, its added private default constructor and the synthetic
constructor flag change. Every other metadata and Exceptions value remains
exact after that one descriptor mapping. Frozen builder and reflection callers
check 64 fresh JVM cases twice, actual collection/context/probe identity,
partial final-field visibility, mutable callbacks and exact failures. Eight
independently compiled wrong programs must fail. Only authored code executes.

The stateful-marker variant reuses javac's anonymous access marker for an enum
switch table. The enum is an owned dependency in both modes. Frozen observers
load the marker without initializing it, then invoke the generic constructor
before or after successful and failed initialization. Passing the unused marker
must not initialize its static state, even after its initialization has failed.

The generated rebuild recipe restores the switch field's synthetic flag after
javac, using its named member binary identity. Its original static/final flags,
initialization, all Code and other class bytes remain unchanged. The frozen
observer rejects an omitted restoration; invalid manifests must fail without
changing any class, and repeating the exact recipe is idempotent.

The `--checked-throws` profiles retain the original IOException declarations on
the private constructor, synthetic access constructor, builder and callback.
The owner implements the same nongeneric callback interface, exercising source
and SDK interface declarations without changing its constructor inputs.
They exercise both empty and stateful markers, preserving the checked failure
object, callback order and partially initialized fields. Frozen reflection
checks the exact declared exception arrays as well as execution. Compiler
metadata comparison covers every original Exceptions attribute, with ten wrong
programs for an empty marker and fifteen for a stateful marker. Native tests
also cover two closed carrier inputs, wide words, broader caller declarations,
unknown or cyclic exception and interface hierarchies, and generic throws that
remain rejected. Existing overload-separation negatives remain unchanged.

The `--nested-owner --stateful-marker` profiles use a private static member
whose access constructor shares the outer owner's anonymous switch marker.
Both original and rebuilt programs use frozen builder and reflection callers;
reflection verifies private/static nesting and reads the partially initialized
member's fields. Checked and unchecked source/SDK variants compile every Java
unit and compare all original definitions, Code roles, Signatures and Exceptions.
They run 72 fresh JVM cases twice with fourteen or sixteen independently
compiled wrong programs, including changed private-member visibility. Native
cases cover high/wide frames, empty or stateful markers and checked contracts;
missing or external enclosing owners, foreign markers, open scopes, instance
state and changed forwarding words remain rejected.
