# Standard long conversion and scoped reference result

The original bridge checks Number, calls its virtual longValue once, forwards
the complete wide value to apply(long), and returns its original S result.
Only the owned raw Function Signature and bridge/synthetic flags are changed;
all Code, descriptors and the target's (long) -> S Signature remain unchanged.

The JVM oracle compares long edges and non-Long Number implementations,
including an authored effectful longValue override; nullable result identity,
typed calls without added conversions, exact ordered failures, and target
generic reflection. A Signature-only result change from S to V must fail
despite their identical Object erasure and unchanged Code.
