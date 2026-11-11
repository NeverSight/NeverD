# Private instance method names

The unmodified Kotlin compiler emits a private final mangled method on an open
generic owner and a same-named public method on its child. The parent call must
retain private direct dispatch; the unrelated public virtual slot gets its own
source family name. An original legal private-helper name reserves a collision.
The optional source annotation profile adds the real compiler synthetic flag.
No classfile or DEX bytes are edited.

A frozen observer checks direct wrappers, reflection and method handles against
both receiver classes, long overflow boundaries, null receivers, repeated state,
partial writes and exact exception identity. All original runtime entries and
metadata are reconciled except disclosed names and the optional synthetic flag.
The original generic owner and Kotlin runtime metadata remain exact. D8 CLASS
annotation loss is independently recorded at two method sites. Three compiled
wrong Kotlin programs must fail; native tests reject foreign/facade private
references, virtual dispatch, changed word order, unknown instructions and metadata.
