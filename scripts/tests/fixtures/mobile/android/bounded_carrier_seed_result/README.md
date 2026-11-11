# Bounded carrier with direct seeds and a formal result

The original Java compiler produces a public static method with one nominally
bounded formal, an invariant carrier of that formal, a long auxiliary and three
direct formal inputs. It forwards to the original virtual carrier member and
returns that member's actual result. Frozen direct, reflective and method-handle
callers check identities, nulls, full long multiplication, exception identity
and effect order. Wrong result, narrowed time, swapped inputs and reordered
result writes are independently compiled counterexamples. Owned and dependency
profiles compare complete original entry and metadata inventories.
