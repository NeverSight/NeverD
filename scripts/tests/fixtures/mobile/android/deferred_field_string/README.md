# Strings after deferred allocation reads

D8 emits allocation, a volatile field read, a string constant and the constructor
call, while the constructor consumes the string before the field result. The
fixture independently verifies that exact encoding for both static and instance
reads. Frozen direct, separate-caller, reflection and MethodHandle paths check
class initialization, captured field identity, null priority, constructor effects,
checked failures, initialization poisoning, actual returned objects and the live
receiver after allocation. Source and SDK profiles compile every recovered source
and retain original definitions and metadata. Fourteen independently compiled
wrong programs qualify the runtime checks.
