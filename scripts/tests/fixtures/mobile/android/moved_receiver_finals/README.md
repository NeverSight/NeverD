# Moved receiver allocation defaults

Independently authored Java uses an anonymous implementation with ten unwritten
primitive/reference fields. The runner adds only witnessed final access flags,
retaining every original Code attribute and descriptor. A twenty-input static
preparation call keeps enough registers live for D8 to copy the constructor
receiver. The runner witnesses an equivalent prefix layout: move the unchanged
copy before the empty Object call and use that exact alias as its receiver.
All later addresses and other DEX bytes stay unchanged, except integrity fields.
The SDK disassembler verifies both the original and the encoded instruction shape.

A frozen observer checks defaults during the original preparation callback,
reflectively changes the integer slot before construction finishes, and retains
the partially initialized object when the exact callback failure escapes. Both
normal and abrupt paths preserve that mutation, publication, argument sum and
single callback. The existing typed-default observations retain raw floating bits,
wide values, reference identity and repeated reflected field updates. No private
sample bytes or source are used.
