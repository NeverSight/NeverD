# Partial final constructor initialization

Authored witness for bytecode that leaves a final field at its allocation
default in a primary constructor and writes it after a delegated constructor
returns. Encoding adds only the field final modifier and independent member
flags; every Code attribute, descriptor and constructor boundary is unchanged.
Both Object and a bundled interface descriptor are exercised; the latter field
also retains its synthetic flag. The frozen oracle checks direct private construction, delegated construction,
receiver publication before stores, typed defaults, final modifiers, callbacks,
partial writes and exact failures. Compilation must be followed by the emitted
field metadata restoration step before application classes execute.
