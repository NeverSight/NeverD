# Stateful static local covariance

An original javac anonymous class inside a static factory implements a plain
interface with a covariant reference return. Its private final array state is
initialized by the original constructor; exact forwarding must regenerate its
original broad bridge. The class is checked both through owned and unchanged
dependency interface/result declarations.

Frozen original calls, reflection over both runtime slots and method handles
compare seed and returned object identity, nulls, index exceptions, primitive
raw bits and ordered failure effects. Source class-name and enclosing metadata
changes must be disclosed; original method descriptors and bridge flags remain
subject to independent compiler checks. Four wrong programs are required to
fail the independent observer.

A second variant is compiled by Kotlin with a genuine enclosing method formal
used only by the anonymous constructor parameter. Its class, fields and
inherited methods remain closed. The recovered local constructor keeps that
formal in source, with the same runtime erasure at ordinary and nested
allocation arguments. Original Kotlin Metadata is preserved; D8 CLASS
annotation loss is checked separately.
