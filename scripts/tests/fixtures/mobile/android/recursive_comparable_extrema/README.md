# Explicit Object erasure with a recursive Comparable bound

Independently authored Java8 selectors retain Object & Comparable<? super E> and
Collection<? extends E>. The frozen observer checks actual comparison order,
returned reference identity, empty/null inputs, runtime/Error identity, duplicate
maxima and a singleton non-Comparable value. That singleton distinguishes the
original Object erasure from an invented early Comparable cast. Every source/SDK
output unit compiles with original metadata, roles and helpers accounted for.
Private application bytes and behavior are excluded from these fixtures.
