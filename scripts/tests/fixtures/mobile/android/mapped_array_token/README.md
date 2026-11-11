# Scoped mapped array token

Independently authored `fixture.*` classes retain an opaque Object input,
invariant `Class<R>` token, `Function<? super T, ? extends R>` and `R[]` result.
The Object input contributes no formal. The token and callback output agree on
R; neither proves the provider's actual array component, contents or identity.
The provider snapshots arguments/token name, invokes the mapper and action,
then reads an opaque array result. Callers preserve actual array casts.

Seven routes and twenty modes run twice in fresh JVMs for source and dependency
profiles. Observe nulls, independent/aliased/covariant/nested/primitive arrays,
callback mutation, exact errors, token/argument snapshots, narrowing, ignored
calls, generic relays and inherited static aliases. Compile every emitted Java8
unit and audit every original definition, Signature, role, Exceptions and helper.
Independent wrong programs exercise the observer before recovery; Signature-only
negative controls retain all Code and runtime descriptors.
