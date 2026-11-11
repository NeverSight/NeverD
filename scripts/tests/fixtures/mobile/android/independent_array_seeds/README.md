# Independent seeds with a closed array

Owned unary generic helper with a plain Object-array input and result.
A frozen observer checks all original Java8 output through direct, separately
compiled, reflection and method-handle calls. It witnesses original array
mutation, null seeds, input/result aliases, initialization, partial effects and
exact RuntimeException/Error identity. No provider behavior is inferred by the
generic binding. Native cases also cover three, four and eight independent
formals, arbitrary scalar contexts, source/SDK providers and invocation frames.
