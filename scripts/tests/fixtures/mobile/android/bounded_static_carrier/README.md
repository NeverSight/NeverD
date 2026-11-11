# Checked static return with a recursive class bound

The independently compiled baseline has a final `Carrier<S extends Base<S>>`
with one private final Object field, an exact constructor, and wrapping and
unwrapping methods. Its static accessor returns `Base<?>` after checking an
unchanged incoming Object. The fixture changes only the accessor's Signature
to `(Object)S` and the three wrapper methods' synthetic flags. Every original
Code attribute is compared byte for byte, then D8 produces the input.

The recovered static accessor uses raw Base while retaining the recursive
generic bound on both classes. The report records the original Signature and
the exact method containing the checked return. The JVM oracle checks two
different recursive subclasses, mutable values, wrapping and returned object
identity, read and prefix exception identity, sentinel checks before prefix
failure, prefix failure before casts and null checks, invalid inputs and null
wrapper receivers, original runtime descriptors, and the generic return
reflection difference.
