# Array component type tokens

This owned fixture keeps two independent method formals in an array copy:
`<T,U> T[] copy(U[], int, Class<? extends T[]>)`. The array input does not
establish the result component; the complete type token binds the result.

Original and rebuilt programs run in fresh JVMs through direct calls, method
handles, and a discarded-result entry. The independent observer allocates with
reflection and copies with `System.arraycopy`; it does not use `Arrays.copyOf`.
It checks runtime array classes, element identities, padding and truncation,
null and invalid inputs, covariance failures, argument identities, ordered
provider effects, mutable inputs and results, and exact checked/Error identity.
The source-provider and declaration-provider profiles freeze original class
metadata, generic signatures, exceptions, observer bytecode, and DEX bytes.
