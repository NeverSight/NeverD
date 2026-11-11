# Abrupt constructors with blank final fields

Unmodified Java 8 constructors either throw without assigning any final, assign
only one wide final before throwing, or return after assigning every final.
A superclass publishes the partially constructed object before callbacks.

Frozen callers use direct calls, reflection and exact method handles. They
observe original default values, reflective mutations, wide stores, exception
construction and checked/runtime/Error identity. Source and SDK layouts compile
every emitted unit and compare every original definition and compiler role.
Independent wrong programs qualify the observer; native malformed paths retain
the normal-return, single-store, receiver and exceptional-flow requirements.
