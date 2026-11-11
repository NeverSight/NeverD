# Original Number float conversion and contravariant reference input

The bridge retains its original Number cast, virtual floatValue call and
checked Box input before calling apply(float, Box<? super Boolean>).
Only the owned raw Function Signature and bridge/synthetic flags are changed;
all Code, target signatures and runtime descriptors remain unchanged.

The JVM oracle compares raw floating point bits, signed zero, infinities,
NaN payloads, non-Float Number dispatch, input/result identity, typed calls
and ordered exception effects. Changing only the target's Box variance from
super Boolean to extends Boolean must fail despite identical erasure and Code.
