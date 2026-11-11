# Original bridge to a differently named generic setter

The independently authored final Child<S,V> extends Base<S>. Its original
set$contract bridge invokes set(S) on the unchanged receiver and returns void.
The runner edits only the owned forwarding method's Signature attribute and
bridge/synthetic flags; all Code and runtime descriptors stay unchanged.

The JVM oracle checks direct and inherited virtual calls, null and object
identity, target effects, exception identity and complete parent/target generic
reflection. Bridge flags are the sole reported reflection change. A target-only
S-to-V Signature mutation must fail despite unchanged erasure and Code.
