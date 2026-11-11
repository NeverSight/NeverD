# Closed local forwarding constructor

An owned static method creates a field-free anonymous subclass with a closed superclass type. Its descriptor-only constructor forwards an integer, an erased class token, and another integer to the original generic parent constructor in declaration order. The parent stores every original argument before recording its entry and suffix effects.

The immutable observer checks the two scalar values, exact class-token and instance identities, class initialization, partial construction, constructor effects, null and unrelated class tokens, and exact failure identity over repeated attempts. The source and dependency profiles compare every original definition and compiler metadata through the single explicitly reported anonymous-to-named-local binding. No original method may be omitted or gain a different generic signature. Independently compiled wrong programs must fail the observer.

The only projected entries are the original anonymous constructor and the exact static factory that declares and allocates it. Both must carry the same reported anonymous-to-named-local role; their rebuilt definitions and metadata are checked through that single class binding.
