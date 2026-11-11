# Original named interface bridge with fixed and bounded parameters

The authored bridge calls Number.floatValue for both original Object inputs,
then apply(Context,float,float,Callback<? super Float>,Box<Float,Vector>).
Its generic interface includes a second formal bounded by a known class.
Only the owned raw interface Signature and bridge/synthetic flags are encoded;
every original Code attribute and descriptor stays unchanged.

The JVM oracle compares ordered conversions, raw float bits, non-Float Number
implementations, fixed reference identity, nullable callback/box/result values,
typed calls and exact exceptions. A Box<Float,Vector> to Box<Float,Bound>
target-only Signature change is rejected despite identical erasure and Code.
