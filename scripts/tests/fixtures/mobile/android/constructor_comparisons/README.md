Owned original Java8 constructors exercise long/float/double comparisons,
signed zero, both NaN outcomes and wide source words. A chained constructor
uses two effectful comparisons, lazy identity or double-capturing allocation,
and a final-field target. Frozen callers compare reflection and MethodHandles;
observers check original input bits, allocation identity, effect counts and
partial runtime/error failures. No input code or metadata is rewritten.
