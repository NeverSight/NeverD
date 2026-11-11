# Inherited scalar factories

The eight scalar profiles implement a closed Producer/Consumer interface graph
through the original named scalar targets, Carrier.readBits and Carrier.box.
The original producer bridge passes its fixed Token argument, receives the
scalar, calls the static factory and returns that exact result. Factory calls
may allocate, return a cached object or null, and throw after observable effects.

Only the owned raw interface Signature and bridge flags are encoded; original
Code bytes and immutable callers remain unchanged. Source and dependency modes
compare every class, runtime method, field, Signature, access flag and Exceptions
attribute. The dependency mode retains the original interfaces and Carrier.

Six invocation routes compare boundary values, allocation and cache identities,
nulls, converter/factory/target effects and ordered exception identities. Existing
consumer and metadata contracts remain present. Native negative cases reject
wrong factory words, return registers, argument origins, access, scope, throws,
missing synthetic flags and divergent inherited siblings. Encoded class-file
counterexamples retain their original Code while changing semantic contracts.

Each source profile retains seven classes and fifteen methods; dependency
profiles retain two source classes and seven methods. Byte/char/short/int/long
matrices each check 126 values, 120 identities, 133 states and 60 failures.
Boolean checks 69 values, 84 identities, 97 states and 60 failures; float/double
each check 183 values, 156 identities, 169 states and 60 failures.

The additional primitive profiles render owned templates before compilation;
compiled originals and separately compiled callers are then frozen for both
runs. Narrow signed boundaries, unsigned char boundaries and both boolean
values are explicit. Float/double comparisons preserve raw bits for finite
extremes, subnormals, both zeros, infinities and payload-bearing NaNs. Only the
original conversion and factory calls establish behavior; no wrapper layout is
assumed. Helper inventories are independently limited to original classes'
throw helpers.
