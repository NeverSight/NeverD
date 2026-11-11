# Receiver-formal getter results before super

Owned generic getter returns the exact type argument supplied by its incoming
receiver. A child constructor supplies that result to its generic superclass
while keeping a separate key and nested Map contract. All Java25 output is
compiled and compared to frozen Java8 observer cases across four entry routes.
The observer witnesses actual calls, mutable getter state, returned snapshots,
null receivers/results, initialization, partial final fields and exact errors.
