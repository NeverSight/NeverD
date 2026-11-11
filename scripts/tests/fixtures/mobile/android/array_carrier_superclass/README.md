# Array carrier superclass bridges

The independent Java input declares `Parent<C,R>` with protected abstract void,
integer and reference-result methods. A final owner retains original public
erased bridges that cast one incoming parameter, invoke its array unbox method,
and call a differently named protected delegate. Fixed reference, integer and
wide parameters stay unchanged. The original bodies are authored Java; only
the original class Signature and bridge flags are encoded before freezing.

The source plan keeps the complete parent arguments and target visibility.
Three separately reported typed entries contain those original bodies; javac
regenerates the original erased descriptors and bridge flags. All original
class, field, method Signature and Exceptions metadata is compared, alongside
an independent expected helper inventory. Added entries and forwarding frames
are reported separately from recovered original methods.

Source and unchanged dependency profiles run the same immutable six-route
JVM harness: direct, parent, reflection, method handles a frozen caller and a recovered source caller.
Each compares 310 values, 408 object identities, 252 states and 198 expected
failures. Null/wrong-type inputs, null receivers, unbox/target exceptions, wide
arguments, nullable array/result aliases and mutable arrays are included.
Same-Code parameter/result arguments, bridge flags and target visibility must
reject, as must an independent javac parameter-contract counterexample.

The source profile contains six classes and twenty-three original methods; the
SDK profile contains five classes and sixteen original methods. Native cases
also cover all primitive array components, nested/reference arrays, void,
integer/wide/reference/array results, public/protected delegates, dependencies,
linked/direct inputs and class/member ordering. Changed register origins,
conversion declarations, scopes, metadata payloads and wide words reject.

Recovered callers evaluate original arguments once, check a null receiver before
a possibly failing carrier cast, and invoke the proved typed entry. The oracle
combines a null receiver with an incompatible argument to protect this order.
