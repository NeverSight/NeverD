# Protected generic array transfer

Authored anonymous regression for the nonfinal protected virtual method
`<T> T[] read(T[] input)`, whose descriptor is
`([Ljava/lang/Object;)[Ljava/lang/Object;` and Signature is
`<T:Ljava/lang/Object;>([TT;)[TT;`.

`Base` and `Subject` use distinct prefix and suffix callbacks. Each method stores
the input array in `saved`, writes `Trace.value` to element zero, captures either
the input or `Trace.replacement`, invokes the suffix, and returns the captured
array. `Core` exercises array, Object, discarded, and checked String-array uses;
the checked use invokes its own callback before casting. `Trace.observe` is the
only abstract declaration. All inputs are authored Java compiled with release 8.

Source mode contains 4 classes and 12 methods: 11 bodies and 1 declaration.
`--dependency-provider` supplies the original `Base` externally, leaving 3 source
classes and 10 methods: 9 bodies and 1 declaration. The recovered definitions,
class and method metadata, and Exceptions attributes must match the originals.
Only the reported static throw helpers may be added: 4 in source mode and 3 in
provider mode. Class source bindings and generic Signature projections are
forbidden.

Each runtime is checked with 4 invocation routes, 4 result uses, 16 modes, and 2
receiver classes: 512 fresh JVM cases with 2 attempts each. The routes are an
immutable separately compiled caller, reflection, method handles, and direct
calls. Baseline and rebuilt runtimes each execute all 512 cases. The modes are:

| Mode | Observation |
| --- | --- |
| 0 | Object-array input and normal write |
| 1 | Null receiver |
| 2 | Null input after the saved-field store |
| 3 | Empty input after the saved-field store |
| 4 | String-array input and compatible write |
| 5 | ArrayStoreException after the saved-field store |
| 6 | Checked prefix failure |
| 7 | Error suffix failure after both stores |
| 8 | Independent String-array result |
| 9 | Null replacement result |
| 10 | Prefix changes the value and replacement selection |
| 11 | Suffix changes globals after result capture |
| 12 | Error prefix failure |
| 13 | Checked suffix failure after both stores |
| 14 | Checked postcall failure before the caller cast |
| 15 | Suffix changes the saved field after result capture |

The frozen observer checks exact callback order, receiver and array identity,
array elements, partial field and array stores, Throwable identity, and later
cast behavior. Thirteen separately compiled wrong programs must be rejected:
wrong receiver, lost prefix, lost saved store, lost array write, wrong array
value, early saved store, lost suffix, late result capture, wrapped prefix
failure, lost override, early caller cast, lost postcall callback, and lost
discarded call.

Eight negative controls change only the Signature of `Base.read`, in this order:
unknown bound, wrong bound erasure, unknown input formal, unknown result formal,
extra formal, declared throw, scalar result, and scalar input. Their original
Code bytes must remain unchanged. In provider mode the altered declaration is
supplied in an external jar. Every control must fail closed without publishing an
output directory.

The runner freezes compiled original and harness hashes, CLI and input hashes,
and the source DEX hash. `baseline.json` contains 512 identical successful
observer receipts; the rebuilt receipts must match it exactly. Its expected
SHA-256 is `c8da3eecb56f38e3b5a9eb733a76358da75a8beb3b68e5c23846b479323957c4`.
All frozen inputs must remain unchanged through the final evidence check.
