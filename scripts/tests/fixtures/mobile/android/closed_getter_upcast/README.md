# Closed getters and a checked generic interface upcast

An owned Java25 constructor evaluates two original getter calls, snapshots
their results and retains an original check-cast before its generic superclass
call. The generic result combines a receiver formal and a closed nominal tail,
then widens through a complete generic superclass/interface chain. Frozen
observers witness nulls, effects, result/receiver identity, initialization,
partial final fields and exact RuntimeException/Error behavior.
