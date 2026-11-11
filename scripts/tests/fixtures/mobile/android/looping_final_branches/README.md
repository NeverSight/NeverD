# Ordered final writes across an unprotected constructor loop

Both constructor arms assign a final reference array followed by a final String.
The empty arm keeps live shared values. The other arm clones array elements in a
loop and calls an opaque text producer after committing the array. No exception
handler protects either arm. The existing ordered field-event proof must retain
normal and abrupt control flow without inferring cloning, result identity or
producer behavior.

Frozen source/SDK observers check escaped partial objects, exact clone references,
mutable inputs, intermediate array/text states, varargs reflection, returned
snapshots, null/cast failures and checked/Error identity. Wrong Java programs
must fail the independent oracle. Original signatures, flags, class/method
identities and Exceptions remain unchanged.
