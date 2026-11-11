# Protected consumers with bounded and mixed parameters

A final Core specializes a protected callback through Base<B> and
Middle<B extends Builder>. Its original compiler bridges accept Object and
Builder, and both forward to the existing Special implementation. Middle keeps
its concrete protected fallback and original bridge; the final Core overrides
them in every observed route. The input
encoding changes only those two Core bridges from protected to public.

The callback also accepts a Token, int, boolean, long and double. An immutable
caller and harness compare nine inherited, typed, reflective and method-handle
routes, including nulls, invalid erased inputs, identity, state and exception
ordering. Signed wide boundaries, negative zero, infinity and NaN check that
original argument bits remain unchanged. Core's typed access changes to public
are disclosed; both original bridge entries and all other metadata remain.

The full original/rebuilt class, method and Exceptions inventories are compared.
Base, Middle, Builder and Token also run as immutable dependency declarations.
Same-Code counterexamples remove either bridge's synthetic flag or alter the
parent argument. Independent javac rejects an incompatible specialization.
Native cases cover source order, bound erasures, copied casts, wide register
pairs and malformed incoming argument origins.

The source profile retains six classes and thirteen methods, with the three
original bridges and one reported typed access change. The dependency profile
retains two source classes and five methods. The runtime matrix checks 864 values,
951 identities, 1,446 state observations and 681 ordered failures.
