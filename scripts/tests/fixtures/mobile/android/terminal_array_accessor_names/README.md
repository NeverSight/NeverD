Owned Java regression for primitive-array getter names beside an erased terminal
generic consumer. The input encoding changes only the complete interface
Signature, bridge/synthetic flags and getter UTF8 names with symbolic callers;
all original Code attributes remain unchanged.

Frozen callers observe byte, short, int and long array identity, null and empty
arrays, payloads, later mutations, evaluation order, exact RuntimeException/Error
identities and parameter-independent fresh terminal exceptions. Source and
imported Api/Marker/Trace layouts compile every emitted Java unit, account for
all original declarations and Exceptions, and disclose only the exact names,
synthetic/bridge flags and raw interface Signature. Independently compiled wrong
programs must fail the observer before decompilation runs.
