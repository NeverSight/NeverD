# Same owner marker delegation with a generic parent

A final Core<T> extends an independently declared Parent<T>. Its private typed
constructor and raw public marker entry retain the original owner formals and
complete parent edge. Frozen callers verify parent initialization before child
publication, callbacks, final field state, wide values, nulls and ignored marker
identity. Only the encoded marker constructor's reported synthetic flag changes
in source; Code, all entries and other metadata remain original. Four independent
wrong programs must fail in owned and parent/interface declaration configurations.
