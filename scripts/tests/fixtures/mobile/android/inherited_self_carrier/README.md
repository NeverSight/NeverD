# Inherited self carrier results

Run `test_mobile_android_inherited_self_carrier.py`, optionally with
`--dependency-root` for unchanged carrier/view/interface declarations.
The carrier has two Object-bound formals, no own members or fields, and no
ancestor use of either formal. The provider returns that carrier parameterized
with its own formal and self type. The interface uses the same formal and its
interface self type. A classfile-only encoding removes the interface wildcard
before all input and declaration hashes are frozen; every original Code and
runtime descriptor remains unchanged.

Java invariance requires a raw source provider result. Recovery keeps complete
original contracts in the common semantic plan, composes provider bindings for
inherited consumers, and reports this single generic Signature change. Original
class/field/member metadata and exceptions otherwise recompile unchanged.
There are no copied provider methods or compiler bridges.

The immutable harness compares original typed caller, interface, reflection,
method-handle and independently overridden routes; nullable mutable carrier
identities, provider effects and original failures are observed. Wrong repeated
formals are rejected on unchanged Code. A separate javac negative demonstrates
why the original invariant metadata cannot be emitted literally as Java.

The source profile retains six classes and seven original methods; the SDK
profile retains three input classes and five original methods. Both compare
135 values, 285 identities, 150 state observations and 20 ordered failures.
The one disclosed result Signature projection keeps coverage `partial`.
