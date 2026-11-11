# Package-visible constructor delegation with a parent

This authored fixture has an open package-visible class whose constructor delegates
to its private overload. Two virtual reads and three distinct generic carrier
allocations precede the original call. The selected constructor forwards all
three closed carrier types to an abstract public parent, then stores its own
arguments. One carrier contains an original raw Class type argument.

The source and SDK profiles verify every original class and method, compile all
recovered Java, and compare 88 fresh JVM scenarios with two attempts each. The
observer checks argument evaluation order, changing provider values, distinct
allocations, parent initialization, partial fields and original exception identity.
The SDK profile excludes only the parent declaration. Deliberately wrong programs
must fail the observer, and Signature-only controls must fail recovery while
preserving all original Code attributes.
