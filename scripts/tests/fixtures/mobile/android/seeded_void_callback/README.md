# Converter, three seeds and closed callback completion

One Object formal connects an invariant bounded converter, three direct seeds,
an invariant specification and both callback inputs. The callback and completion
share the same closed result type. Frozen callers verify input identities,
conversion/specification reads, callbacks, original result casts, wide bound
values, nulls, partial state and failures. All original compiler metadata and
entries remain intact. Owned and SDK variants run 56 cases twice per stage;
four independently compiled wrong programs must fail.
