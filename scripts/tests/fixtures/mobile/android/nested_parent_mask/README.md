# Raw masks under a nested generic parent

The first-party Java 8 program has a package final Core<T> extending
Parent<Local<T>>. Local is an owned package type with a two-level nominal path
to Parent's bound. Parent also implements a plain interface. The complete typed
constructor delegates to that parent; a raw constructor selects defaults from
mask bits before same-owner delegation. An explicit class-file witness adds only
the mask entry's synthetic flag, leaving Code, descriptors and all other bytes
unchanged.

Frozen package callers, reflection and method handles compare 256 fresh JVM
cases twice, including both default bits and ignored high bits, count/long
extrema, lazy provider initialization and sticky failure, every failure stage,
null callbacks/tokens and partial parent/final fields. Seven incorrect programs
must fail those observers. Owned and SDK parent/bound/interface variants compile
all generated Java and check every original definition and metadata field, with
only the reported synthetic constructor flag difference. Native tests reject
foreign/inaccessible carrier types, missing bounds, wildcard or unused owner
formals, raw arity changes and changed invocation words.
