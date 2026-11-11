# Abstract generic owner access constructor

An abstract Core<T> has a private typed constructor and one raw public marker
constructor whose Code transfers every original argument unchanged. Only its
synthetic flag is encoded after frozen callers compile, with a byte-level
witness. A real derived class exercises virtual dispatch during construction,
callbacks, partially initialized parent/child fields, wide values, nulls,
marker identity and failure order. All class/method metadata stays unchanged
except that source reports the synthetic flag loss. Four independently compiled
wrong programs must fail in owned and dependency declaration configurations.
