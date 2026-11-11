Independently authored final-owner terminal-throw result adapter. Before the JVM
baseline, bounded class-file edits attach the full parameterized superclass,
rename the owned typed entry, and mark its erased boolean bridge; every Code
attribute and runtime descriptor stays unchanged. Recovery preserves all class
generics and ordered helper/allocation/throw behavior, projects only the typed
Void result to boolean, and reports its changed descriptor and generic result.
Known narrow callers retain their original Void result and always throw before
the source boxing can execute. Equal-parameter collection and class-formal
Object-erased entries share one source boolean method; the report identifies
their changed generic return, bridge flags and forwarding stack frame.
Signature-only argument drift and an independent
original-body normal-return counterexample must fail recovery.
