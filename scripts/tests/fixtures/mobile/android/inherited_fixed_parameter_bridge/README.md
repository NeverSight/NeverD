# Inherited fixed parameter bridge

Owned source and SDK fixtures retain a concrete parent implementing two methods
of a closed generic interface. The child overrides only `read`; `extra` keeps
both original parent declarations and acquires no child method or bridge.

The frozen caller and future subclass exercise typed, erased, generic and special
inherited bridge entries. Original receiver effects, checked parameter casts,
virtual redispatch, callback order, result identity and exact exception objects
must survive recovery. All regenerated classes and methods retain their original
metadata and roles. Runtime tests execute only these owned fixtures.
