# Primitive parameter bridges

Owned declarations retain a fixed generic superclass contract through a literal
parent edge. A fieldless anonymous static initializer subclass has original
Object-to-Value getter and setter bridges, including a float return. The source
owner uses the common named anonymous-member mapping; original bodies, casts and
primitive results remain checked independently.

The frozen JVM observer checks raw bridge dispatch, null and failed casts, float
bit patterns (including a NaN payload, signed zero and infinities), snapshots,
input identity, field effects and exact failures. Source and dependency-parent
profiles audit every original definition and compiler fact, permitting only the
reported anonymous name and member metadata changes. Wrong programs must fail
before recovery. Only owned code is executed.
