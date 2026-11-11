# Unwritten instance Boolean final

Compiler original constructors never store the zeroFlag Boolean. A witnessed
field-access-only edit adds final; Code, descriptors and all other attributes
remain identical. Frozen observers check allocation defaults, partial values,
loops, failures and reflective mutation during construction. This prevents
regenerating the field as an inlined constant. No application data is used.
