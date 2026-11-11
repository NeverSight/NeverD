# Checked forwarding with one final reference wildcard

The authored raw Receiver receives an owned implemented-interface Signature
whose Mapper return argument is ? extends Value; Value is a known final class.
The original target parameter retains invariant Value. Only owned class
Signature and bridge flags are encoded; every Code attribute stays unchanged.

Recovery widens that single source target parameter to the exact inherited
wildcard after full nested type identity and original bridge forwarding are
proven. The JVM oracle checks input/result/field identity, nulls, typed and
erased dispatch, callback effects, failure order and reported target reflection.
A target-only change to another final class is rejected with identical Code.
