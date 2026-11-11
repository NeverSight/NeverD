# Producer carriers and callbacks with a closed completion tail

Owned Java keeps one Object-bounded element in a covariant input carrier and
the same invariant result carrier. The binary callback consumes that element
and a lower-bounded completion of Boolean, returning an opaque Object. Provider
behavior remains opaque: it can complete, throw or return a different carrier.
The frozen observer checks snapshots, nulls, bridge failures, exact RuntimeException
and Error identity, initialization caching, inherited static lookup and every
returned carrier reference. No filtering or coroutine scheduling is inferred.
The owned Client contains the actual inherited generic invocation in DEX.
