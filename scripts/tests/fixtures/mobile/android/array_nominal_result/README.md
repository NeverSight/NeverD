# Array inputs with fixed nominal results

Owned Java classes exercise a public static generic method inherited from a package-private backend through a public facade. One Object-bounded method formal belongs to the original array component; the result is a separately declared nongeneric type. The original provider may return null or an opaque selected object and may mutate or reject the original array.

The frozen observer checks static member lookup, overload selection, declaring-class initialization, checked exceptions, Errors, null arrays, covariant array stores, input and result identities, callback ordering, changing provider results, and caller effects after return. Both source and dependency-provider profiles retain all original definitions and full generic declarations. No class or DEX patching is used.

A generated source variant gives the backend an unrelated Number-bounded class formal with the same name as the static method's Object-bounded formal. The array component must keep the method scope, including through the raw public facade; both full declarations are preserved.

A Kotlin variant returns a nongeneric class implementing a recursive Comparable interface through an original checked primitive bridge. Its independently compiled observer exercises the returned object through reflection, method handles and a frozen interface caller, including null and incompatible Comparable inputs. The original header remains the authority for the nominal result while the report records the raw source interface and bridge flag differences. Four additional wrong predicate programs must fail the observer. Only known CLASS annotations lost during D8 conversion and Kotlin source filenames are normalized; all other compiler metadata is compared exactly.
