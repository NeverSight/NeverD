# Static Kotlin method enclosures

The unmodified Kotlin compiler emits a mangled static method, its synthetic default
helper and an anonymous class allocated by that helper's default argument. The
anonymous class's EnclosingMethod names the original default helper. No classfile
or DEX edits are used. A frozen caller/observer compares original and recompiled
execution, including reflection against the exact disclosed source helper name.

The default and explicit argument cases cover fresh object identity, mutable input
aliases, ordered effects, nulls, partial writes, class initialization failures and
exact exception identity through direct calls, reflection and method handles.
All original definitions and metadata are reconciled, allowing only documented
static names/synthetic flags and the named local class's identity/InnerClass flags.
Kotlin runtime metadata remains exact. D8 CLASS annotation loss is independently
read and recorded at its exact sites. Three wrong Kotlin programs must fail the
frozen observer; native tests reject external and named local enclosure metadata.
