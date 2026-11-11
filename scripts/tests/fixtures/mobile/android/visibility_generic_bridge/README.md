# Erased visibility bridges over closed generic inputs

A public class inherits a public method from package classes. The complete
parent declaration takes `List<String[]>`, while javac supplies an erased
public bridge with no Signature and unchanged superclass forwarding. The owned
fixture verifies that source and SDK-parent recovery lets javac recreate this
entry with original public/bridge/synthetic flags, raw reflective parameters
and the original unnamed synthetic MethodParameters table.

Frozen original definitions and observers compare foreign calls, reflection,
method handles, subclass overrides and superclass calls. The independent oracle
checks argument/receiver identity, array and list mutations, late casts and
bounds/null failures, actual return snapshots and exact checked/Error identity.
Original class metadata, full parent Signature, Exceptions and every method
identity remain unchanged; only reported runtime throw helpers may be added.
