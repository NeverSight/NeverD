# Allocated unary owner constructor

Original source constructs Mutable<S> from its exact S input before upcasting
to State<S> and selecting the same-owner State<S>/Core<?>/String target.
The original constructor formal and complete inherited edge prove this
binding. Frozen direct, precompiled, reflection and method-handle observers
check seed identity, nulls, target final fields and failure order. Four wrong
programs must be detected; no bytecode or metadata is rewritten.
