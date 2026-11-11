# Private interface seed and result

A private instance method declares one method formal with a single plain
CharSequence interface bound. One direct formal input and a scalar limit retain
that same formal result on its original erased interface descriptor. All source
callers belong to the exact declaring class; a same-class peer receiver still
keeps its original identity and null failure.

The observer distinguishes helper and caller casts: a subsequence may return a
different CharSequence implementation, and only the original typed caller may
then cast it to Text. Null inputs/results, receiver identity, scalar guards,
length/subsequence call order and exact failures remain observable. Runtime
contents or concrete input classes do not bind the method formal. The SDK
variant supplies the owned public abstract Text declaration. Original compiled
subjects and observers are frozen before the fix; every emitted Java unit and
all original class/member/Exceptions metadata are checked.
