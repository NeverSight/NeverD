# Nested static carrier factory result

An owned generic Java class declares a public static nullary factory returning
`AtomicReference<Holder.Slot<T>>`. The nested carrier is an owned private static
class. The enclosing class, factory, private transfer and nested class each
have a separate same-spelled formal. Their exact original Signatures remain
unchanged; the compiler does not infer a formal from runtime reference contents.

The SDK variant supplies only the public Trace declaration. The observer checks
public source callers, reflective factory/transfer calls, fresh allocations,
null defaults and null transfer inputs, arbitrary raw contents/subclasses,
identity, partial effects and exact RuntimeException/Error/checked failures.
Ten independently compiled wrong programs must fail before NeverD runs. All
emitted Java units and complete class/member/Exceptions metadata are checked.
No private application or output is included.
