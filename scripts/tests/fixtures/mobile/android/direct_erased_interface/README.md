# Direct erased implementation with nested wildcard metadata

The independent javac baseline implements a raw two-parameter interface. Its
ordinary `Object map(Object)` method first casts to a parameterized Pair, then
returns the String produced by two exact member calls. A second method returns
a typed Cursor. The fixture adds class and map Signature attributes narrowing
the interface arguments and map prototype; every original Code attribute and
runtime descriptor remains intact. D8 supplies the decompiler input.

Recovered Java keeps the interface raw and the map descriptors unchanged while
preserving the typed cursor method, field and constructor metadata. Direct and
interface calls compare mutable String identity, getter and reader exception
identity and ordering, cast failures before effects, null inputs and receivers,
and cursor identity and position. Reflection confirms the three reported
metadata differences and the absence of a bridge. Negative component cases
reject changed inputs, returns, interface slots and method formals; bounded
uses of the former parameterized interface are rechecked in either declaration
order.
