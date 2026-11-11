# Numeric Comparable predicate bridges

The `--numeric` profile of `test_mobile_android_comparable_scalar_predicates.py`
extends the checked predicate fixture to Double arguments, inherited interface
formals and two-argument scalar calls. The original bridge casts each input to
Number, invokes doubleValue in order, and passes both original wide values to
its original target. The raw interface still accepts other comparable Number
subclasses at runtime, so the fixture also checks Float input and a custom
Number with visible conversion effects and exact exception objects.

The immutable harness checks NaN, infinities, signed zero, comparison boundaries,
left-before-right conversion, duplicate references, exceptions before and after
the target's state changes, null receivers and covariant getter identity through
eight direct, interface, reflective, MethodHandle and original caller routes.
All original runtime slots remain present. Only the two raw interface edges and
two retained predicate bridge/synthetic flag pairs differ in source metadata;
the original covariant getter bridge is regenerated. Native controls reject
wide-word swaps, lost high words, changed incoming origins, wrong move-result
width, missing unbox declarations and divergent or cyclic inherited formals.
