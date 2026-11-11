# Scoped superclass constructors

The caller and parent each declare their own Object-bounded constructor formal,
with independently named direct seed and complete invariant unary carrier slots.
The parent's unrelated recursive class formal remains closed by the original
child class edge. The original first superclass invocation forwards both inputs;
no argument or callback result is inferred from constructor behavior.

Fresh owned JVMs check original input identity, callback and result snapshots,
partial construction state, nulls, exception identity and constructor suffixes.
Source and dependency-parent profiles audit every original definition and
compiler fact without scope projections. Wrong programs must fail the frozen
observer before recovery. Only owned Java is executed.
