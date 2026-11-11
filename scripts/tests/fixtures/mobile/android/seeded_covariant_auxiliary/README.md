# Seeded results with a closed covariant auxiliary

These independently authored classes pair an Object-bounded method formal in
the input and result with a separate `Class<? extends Runnable>` parameter.
Both static and virtual calls retain the original inputs, actual provider result
and original caller casts. The provider also has a plain Serializable interface.

The immutable observer tests eight raw, narrowed, ignored and generic routes,
eighteen input/callback cases and two successive attempts. It checks null
receivers, overriding dispatch, returned and input identities, callback changes,
ordered partial effects and exception identity. A seed never predicts the result.
Source and dependency profiles compare all original class and method definitions,
fields, Signatures, roles and Exceptions. Wrong programs must fail this observer;
Signature-only controls target an entry referenced in both profiles.
