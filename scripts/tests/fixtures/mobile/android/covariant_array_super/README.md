# Nested covariant array components before generic superclass construction

Owned Java 25 inputs and unchanged Java 8 provider bytes retain a complete
`Path<K,V,Map.Entry<? extends K,? extends V>>[]` contract through the original
array construction loop and superclass call. The allocated element subclass
has exactly the same nested covariant superclass edge. Source and SDK profiles
compile all output, compare original signatures and metadata, and use frozen
observers for every ordered allocation, array mutation and failure state.
