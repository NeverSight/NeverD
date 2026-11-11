# Throwing generic factories with ordinary arrays

Independently authored factories bind one Object-bounded formal through an
invariant token, keep an ordinary input array and return an independent value.
Both varargs and fixed-array entries preserve checked and unchecked exception
declarations. The token does not predict the actual result or array contents.

The frozen observer runs six routes and eighteen scenarios twice in fresh JVMs.
It checks original array aliases, retained elements, null/empty arrays, late
result casts, callback mutation, array-store failure and exact checked/unchecked
exception identity. Source and dependency profiles compile every recovered Java
unit and compare original definitions, access, generic metadata and Exceptions.
Incorrect programs must fail the same observer. Metadata controls retain Code
and runtime descriptors while invalidating generic or exception declarations.
