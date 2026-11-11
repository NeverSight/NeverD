# Abstract bounded class parameters

Run `test_mobile_android_abstract_bounded_parameters.py`, optionally with
`--dependency-root` for unchanged Root/Parent declarations in a JAR.
The independently authored generic middle class narrows its class-formal bound.
Only its javac Object-erased bridge is omitted before input hashes are frozen.
Both original public concrete bridges remain in the input and are validated.

Original class, field, method and Exceptions metadata must match after recovery.
The single absent protected bridge is disclosed as a compiler helper; coverage
is partial because reflection, dispatch and stack frames can differ. The
original abstract method remains abstract with its complete class-formal scope.

The immutable JVM oracle checks twelve reflection, method-handle and source
routes on original and independently overridden receivers. It covers nulls,
mutable aliases, long extremes, mixed argument positions, checked casts before
callbacks, callback failure identity and null failures after target effects.
There are 864 values, 2808 identities, 952 states and 100 ordered failures.
Source and dependency profiles retain seven/five classes and fourteen/ten
original methods respectively. Same-Code class/method-formal drift and missing
concrete bridge flags are rejected; independent javac rejects the formal drift.

`--concrete-parent` adds an original concrete implementation to Parent before
javac input generation; Middle still declares its bounded method abstract.
A separately compiled receiver calls the preserved parent body through both
virtual and super routes (144 values, 300 identities, 148 states, six failures).
This profile adds one original source method, or no input method in SDK mode.
It does not treat the parent's input body as a compiler helper.
