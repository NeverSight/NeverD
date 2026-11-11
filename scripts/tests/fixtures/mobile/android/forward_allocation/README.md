# Forward nullary initialization

Owned ASM input retains `new` and its nullary initializer on different forward
blocks inside a try region, physically separated by unrelated switch siblings.
Both independently authored Java and actual ASM classfiles run against the same
frozen JVM observer before D8; the emitted DEX must retain the jump witness.
The fixture is described as authored bytecode, not javac output. Recovery must
preserve allocation, class initialization, partial construction, exact failures,
cleanup and failed initialization caching. Every recovered Java unit compiles;
source/dependency inventories and original metadata are checked.

Run the builder with JDK 21, as configured by its CI profiles. It uses the
bundled internal ASM API only to author this test input.

The shared-cleanup profile additionally authors the exact Dalvik layout in
smali, using the other original classes as dependencies. Its catch path jumps
to a null-return sibling physically between allocation and initialization.
Both the ASM program and independent Java specification execute against the
frozen observer; D8 duplicates this shared return during lowering, so this
profile explicitly uses authored smali rather than claiming compiler output.

The `--argument-tail` source and SDK profiles retain an unconditional forward
jump from allocation to a tail containing a filled reference array and nested
StringBuilder construction. Independently authored Java and ASM inputs execute
against the same frozen caller; D8 must retain the jump, unrelated switch sibling,
array and nested-allocation witnesses. All Java compiles with exact original
class/method/Code/access/Exceptions accounting. The observer checks class
initialization before argument effects, first and repeated failures, captured
array elements, later argument mutation, partial stores and exact catch/cleanup
failures. Ten independently compiled wrong programs must be rejected. Native
cases cover jump encodings, range/wide words, direct and linked order, and
reject extra entries, receiver aliases and malformed array results.
