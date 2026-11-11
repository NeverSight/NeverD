# Reifiable wildcard array delegation

Owned overloaded constructors keep an original File.toPath virtual call and an unchanged Class<?>[] varargs array. The target's complete reifiable source type retains the selected descriptor beside String and Path overloads without inferring an element type or packing another array. No filesystem operation runs.

Four frozen constructor caller routes compare 96 cases twice against source and SDK Trace declarations, including derived dispatch, null receiver/path/array, empty arrays, writes, conversion and target failures and late result casts. Six wrong programs and six Code-preserving Signature controls guard order, type scope and overload selection; every recovered unit is compiled with original metadata.
