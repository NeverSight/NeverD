# Overloaded scalar interface super calls

The original Java 8 program extends the scalar witness with `(I)F` and `(J)D`
overloads beside `(F)I`. The int overload selects Root; float and long select
Right through Api's diamond. Abstract and final owners retain exact interface
super qualifiers, and Child adds an observable override before superclass calls.

The runner's `--overloaded` profile explicitly encodes only illegal method names
and bridge flags; Code, descriptors and other metadata remain unchanged. Frozen
callers, reflection and method handles compare three overloads, two receivers
and 128 fresh JVM cases with distinct failure points. They observe NaN payloads,
signed zero, integer precision, both long words, receiver identity and exact
exceptions. Seven wrong programs cover dispatch, omitted effects, parameter
changes, int-to-float overload selection and lost high long words. Owned and
external Trace variants compile all generated Java and account for every entry,
name change, bridge flag and helper.
