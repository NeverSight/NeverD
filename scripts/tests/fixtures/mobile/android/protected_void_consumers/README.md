# Protected void consumers with public erased bridges

The open generic Core specializes a protected generic Base consumer with a
closed Event parameter. Only the erased bridge access is encoded from protected
to public before freezing originals. All Code, descriptors and other metadata
remain unchanged; Child inherits the original implementation.

Run `test_mobile_android_protected_void_consumers.py`, optionally with
`--dependency-parent` to keep Base in an immutable declaration JAR. The typed
source method becomes public as explicitly reported so javac retains the
original public erased slot. No runtime entry or class Signature is added.

The immutable harness uses typed wrappers, parent callbacks, reflection,
MethodHandles and an independently compiled raw caller. A separately compiled
protected override stays unchanged and must retain virtual dispatch against both
original and rebuilt classes. Nullable mutable inputs, cast-before-target
failures, effects and exception identity are compared. Recompiling that protected
override against the projected public target requires public access, also
reported. Known input descendants that redeclare the typed slot are outside this
bounded profile. Same-Code flag and parent-binding mutations fail; independent
javac rejects the incompatible source override. Coverage remains `partial`.

The source profile retains four classes and nine original methods; the SDK
profile retains three input classes and six original methods. The immutable
oracle compares 216 values, 510 identities, 1,044 state observations and 186
ordered failures. Two original method rows disclose the access/bridge source
projection; all other original metadata remains unchanged.
