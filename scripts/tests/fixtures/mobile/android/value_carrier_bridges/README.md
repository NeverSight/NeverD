# Checked wide-carrier bridges

These independently authored Java classes model two original checked virtual
unboxing calls, a `long,long -> long` instance target and a static boxing factory.
The runner adds only bridge/synthetic flags and the typed implemented-interface
Signature after compiling the harness; every Code byte and runtime descriptor
stays unchanged. No external application code is used.

The JVM oracle compares 48 values, 13 identities and 11 ordered failures, including
wide edge values, factory allocation, target effects, wrong types, nulls and each
callee's original exception. Typed primitive calls do not box or unbox. The report
must disclose the raw interface and changed bridge flags. Swapping the carrier
arguments only in metadata, with identical Code and descriptors, must fail.

The reference and array-Unit variants support `--parameterized-reference
--open-reference` to encode `Carrier<? extends S>` with the receiver's original
formal. Their independently compiled harness checks the full original wildcard
and scope, virtual conversion effects and failures, mutable aliases, and exact
reference/array results. The unchanged conversion declaration has a literal
reference result independent of the carrier formal. Generic conversion results
remain outside that proof. Raw interface and bridge-flag changes are reported;
all other metadata, original inputs, harness and SDK bytes are preserved.
