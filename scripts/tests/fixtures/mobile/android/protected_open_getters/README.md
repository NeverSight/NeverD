# Public erased getters on nonfinal owners

Core has a protected nominal getter and its original erased Broad bridge. Slot
retains its complete generic Broad ancestry, Base keeps its original bounded
formal and Child inherits both runtime slots. After compiling an immutable JVM
harness, the runner changes only the original bridge from protected to public;
all Code, descriptors and other metadata remain unchanged.

Run `test_mobile_android_protected_open_getters.py`, optionally with
`--dependency-parent` to retain Base in an unchanged JAR. Six call routes cover
Core, Child and an independently compiled protected override. Nullable mutable
results, fresh allocation counts, parent callbacks, failure identity, effects
before exceptions, reflection and MethodHandles are compared on original and
rebuilt code: 90 values, 432 identities, 954 state checks and 114 ordered failures
per profile. The source and parent-JAR profiles retain five classes/ten methods
and four classes/seven methods respectively. Full metadata changes only at the
reported target access; no method is copied into Child. The report discloses that recompiling the separate
protected override against the public source target would require public access.

Same-Code bridge-flag and complete superclass-argument mutations fail recovery,
and independent javac rejects the incompatible result. Native controls include
optional result casts, copied self registers, source/SDK roots, ordering and
known source descendants that redeclare the protected getter. Coverage remains
`partial`.
