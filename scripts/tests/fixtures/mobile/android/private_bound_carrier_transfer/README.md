# Private bound carrier transfer

Authored anonymous regression for the private instance method
`<E, S extends Slot<E>> S read(S input)`. Its descriptor is
`(Lfixture/Slot;)Lfixture/Slot;` and its complete Signature is
`<E:Ljava/lang/Object;S::Lfixture/Slot<TE;>;>(TS;)TS;`.

`Slot<E>` is an interface. `Bag<E>` implements its `put(E)` method without a
bridge: both declarations have the original Object parameter erasure. `Core`
invokes its private method from the same declaring class. That method calls a
prefix, saves the input, invokes `input.put`, captures the input or an independent
replacement, calls a suffix, and returns the captured reference. A checked
wrapper retains its own `<E> Bag<E>` return and invokes a postcall callback before
the Bag cast. All original inputs are authored Java compiled with release 8.

Source mode contains 4 classes and 12 methods: 10 bodies and 2 declarations
(`Slot.put` and `Trace.observe`). `--dependency-provider` supplies only the
original `Slot` externally, leaving 3 source classes and 11 methods: 10 bodies
and 1 declaration. The private method remains owned in both modes. Rebuilt class
and method identities, metadata, and Exceptions attributes must match the
originals. Only 3 reported static throw helpers may be added, one each in Core,
Bag, and Trace. Class source bindings and generic Signature projections are
forbidden.

Each runtime is checked with 4 invocation routes, 4 result uses, 16 modes, and 2
input implementations: 512 fresh JVM cases with 2 attempts each. The routes are
an immutable separately compiled caller, reflection, method handles, and direct
calls. The implementations are the original Bag and a separately compiled
harness implementation of Slot. Baseline and rebuilt runtimes each execute all
512 cases. The modes are:

| Mode | Observation |
| --- | --- |
| 0 | Normal write and original input result |
| 1 | Null receiver |
| 2 | Null input after the saved-field store |
| 3 | Null element write |
| 4 | Checked put callback failure before the element store |
| 5 | Error put callback failure after the element store |
| 6 | Checked prefix failure |
| 7 | Error suffix failure after both stores |
| 8 | Independent Bag result |
| 9 | Null replacement result |
| 10 | Prefix changes the value and replacement selection |
| 11 | Suffix changes globals after result capture |
| 12 | Error prefix failure |
| 13 | Checked suffix failure after both stores |
| 14 | Checked postcall failure before the caller cast |
| 15 | Suffix changes the saved field after result capture |

The frozen observer checks exact callback order, receiver and carrier identity,
stored values, partial stores, Throwable identity, and late cast behavior.
Thirteen separately compiled wrong programs must be rejected: wrong receiver,
lost prefix, lost saved store, lost put call, wrong put value, early saved store,
lost suffix, late result capture, wrapped prefix failure, lost put store, early
caller cast, lost postcall callback, and lost discarded call.

Eight negative controls change only the Signature of `Core.read`, in this order:
unknown bound, changed element bound, unknown bound formal, variant bound slot,
extra formal, declared throw, changed result formal, and changed input formal.
All original Code bytes must remain unchanged. The control target stays in source
in both modes; only Slot is supplied in the provider jar. Every control must fail
closed without publishing an output directory.

The runner freezes compiled original and harness hashes, CLI and input hashes,
and the source DEX hash. `baseline.json` contains 512 identical successful
observer receipts; the rebuilt receipts must match it exactly. Its expected
SHA-256 is `87d977067ab4b1aaffbaa155d5e55065f0eefb5c09d5d6320bcf7c55478d47cf`.
All frozen inputs must remain unchanged through the final evidence check.
