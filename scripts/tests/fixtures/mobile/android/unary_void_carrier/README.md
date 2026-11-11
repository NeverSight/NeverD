# Unary and contextual void producers

The ordinary profile checks a generic unary interface method and its virtual
overrides with an independent frozen observer.

`--contextual-static` authors a public static `<T> void` entry receiving a
closed `Provider` observer and `Factory<? extends T>`. The original caller
forwards both inputs unchanged. Reflection checks the complete static method
contract. Source and SDK entry profiles compare every original definition,
metadata and Exceptions and compile all recovered Java.

Each profile checks 80 fresh JVM cases, two attempts each: four caller routes,
base/override dispatch, mutable callback aliases, partial stores, nulls, opaque
Object results, original later cast timing and exact failure identity. Six
independent wrong programs test lost observer/producer inputs, eager/repeated
callbacks, stores and override effects. No private sample data is included.

Adding `--reifiable-setter` authors an invariant `Factory<T>`, direct `T` seed
and independent `List<?>` property input on the void entry. The frozen observer
checks all three complete generic contracts and their reference snapshots,
including nulls and changes to global inputs during callbacks. Eight wrong
programs also detect lost seed/property inputs. Source and SDK modes use the
same original definitions and frozen callers before and after recovery.
