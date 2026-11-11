# Direct seed callback

The original compiler signature adds a direct Object-bound method seed to a
bounded carrier pair, unary spec, Boolean flag, callback consuming the same
pair and completion of its closed result. The frozen observer verifies the
independent seed's identity including null, both Boolean paths, all provider
and callback effects, actual result casts and partial failure state. Four
wrong programs must be detected. No bytecode or metadata is rewritten.
