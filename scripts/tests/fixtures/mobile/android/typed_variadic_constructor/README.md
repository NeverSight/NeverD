# Owned typed variadic constructor fixture

A generic class forwards a complete Class<T> token and the same String array
from a variadic default-false constructor to a variadic boolean overload. The
full class scope owns the token binding; explicit typed array arguments retain
fixed-arity overload selection and original varargs metadata. Source and SDK
marker profiles compile every unit and compare all original entries, fields,
metadata and Exceptions. Frozen callers run 144 fresh JVM cases twice through
four entry routes. They observe exact type/array identity, defaults, null/empty
arrays, mutations, constructor counts, partial final fields, exact failures and
late Class.cast/caller casts. Six wrong programs and six Code-preserving
Signature controls protect scope and ABI. Only owned fixture code executes.
Use JDK 25 for recovered prologues and release 8 for originals.
