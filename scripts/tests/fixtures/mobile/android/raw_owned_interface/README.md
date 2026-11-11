# Original class-owned parameters behind a raw SDK interface

This independently authored comparator has one Object-bound class formal and
no fields. Its original `compare(T,T):int` method erases directly to the SDK
slot, without a bridge. The runner changes only the original class Signature's
interface edge to raw; descriptors and Code remain unchanged. Recovery reports
the unique missing interface argument and preserves all original method scopes,
runtime entries, bodies and SDK default owners.

The immutable JVM harness checks direct, interface and reflected calls, the
unchanged reversed default and a method-generic `thenComparing` default. It
checks object identities, integer results, ordered state changes, original
exception identity and null receivers. Recompilation permits only the reported
class Signature change and disclosed source helpers. Same-Code method-owned
shadowing and inconsistent parameter Signatures must fail recovery and
independent javac. Input, harness and SDK bytes remain unchanged.
