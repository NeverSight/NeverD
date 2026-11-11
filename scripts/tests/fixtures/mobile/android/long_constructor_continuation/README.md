Owned regression for a short constructor argument prefix followed by an
initialized continuation longer than 256 Dalvik operations. Seventy-two
observable steps retain live receiver state and exact callback order.

Frozen callers use direct construction, a constructor method handle, and an
extending class. They verify blank final initialization, partial receiver
escape, virtual dispatch, suffix branches, input mutation, nulls and exact
checked/Error identity. Fourteen deliberately wrong programs must fail the
independent observer. Source and SDK-provider modes retain original inventories
and metadata, apart from reported helpers and the required Java 8 to 25 class
file version change. Only owned regression code executes on the JVM.
