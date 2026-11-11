Independently authored closed self carrier with a short representation. Its
original erased comparison casts the argument, invokes the carrier's original
conversion, and passes that exact short into a differently named instance
comparison. Before the JVM baseline, bounded metadata edits attach the complete
Comparable contract and bridge flags without changing Code or descriptors.

The oracle measures all extreme/signed short combinations, conversion and
comparison effects, receiver identity, conversion/target failures, null
receivers and invalid erased arguments. Recovery retains the complete bridge
body and reports only raw source interface and bridge-flag differences. The
full class, field and method inventories and metadata are checked independently.
