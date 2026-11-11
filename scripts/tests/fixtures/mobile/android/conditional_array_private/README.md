# Conditional primitive array arguments and private calls

The first scalar array element is read after allocation/class initialization,
then an original private call captures a reference, mutates that original array
and replaces the volatile array/reference fields. The conditional tail reads the
new arrays and retains its original guarded fallback. Constructor observations
check every argument and partial field state.

The frozen observer covers null/bounds, skipped fallback, changed/same arrays,
actual references, exact RuntimeException/Error, initialization caching and four
invocation routes with two attempts. The wide profile uses large long values;
the input remains unmodified javac/D8 output in both source and SDK modes.
