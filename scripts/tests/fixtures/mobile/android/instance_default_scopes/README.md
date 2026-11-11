# Instance default helper generic scope fixture

An owned generic receiver has an instance method and an inlined static default
helper. Both call the same declared member on the unchanged receiver. The
helper applies its default mask and creates a noncapturing callback.

The JVM baseline changes only metadata: the helper becomes synthetic without
a Signature, and the callback's Box argument retains the receiver's E formal.
The original method code remains intact. D8 and SDK dexdump supply independent
input inventory. Recompilation and JVM checks cover supplied/default callback
identity, receiver call counts and exception identity, mutable boxed values,
null behavior, original descriptors and the explicit reflection difference.
