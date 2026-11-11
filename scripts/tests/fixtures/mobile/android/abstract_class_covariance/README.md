# Abstract class covariance

Run `test_mobile_android_abstract_class_covariance.py`, optionally with
`--dependency-root` to keep Root and Api in an unchanged declaration JAR.
The owned middle class narrows two abstract class returns. Before input hashes
are frozen, only its public/protected broad compiler bridges are omitted;
every remaining body, descriptor and metadata item stays unchanged. Concrete
child bridges remain present and independently validated.

Recovery preserves the original abstract declarations and reports the two
javac additions as generated compiler helpers, separately from original method
inventory. Recompiled original metadata and exceptions match exactly. Coverage
is explicitly partial; bridge reflection, dispatch and stack frames can differ.

The immutable JVM oracle exercises twelve reflection/method-handle routes on
original and independently overridden receivers, nullable mutable results,
method-formal identity, effects and original exception identity. Same-Code
formal/visibility mutations fail. A separate javac negative confirms that a
same-erasure method-formal change is not a valid override.

The source profile retains four classes and sixteen original methods. The
oracle checks 216 values, 462 identities, 246 state observations and 36 ordered
failures. Six original rows are projected: two abstract declarations with
compiler additions and four original concrete bridges.
The SDK profile retains two input classes and twelve original methods, with
identical runtime observations and the same two reported compiler additions.
