Owned regression for a static boolean generic call with one contravariant
Collection input and the identical method formal as an explicit varargs array.
The complete array is passed at fixed arity. The callee may return either boolean
independently of its container callbacks; callers use or discard that result.

Frozen observers compare array and carrier identity, bridge dispatch, live and
snapshotted elements, null/cast/store failures, partial mutations, exact checked
and Error identity, and callbacks before and after the call. Fifteen wrong
programs must fail. Source and SDK-provider modes retain all original definitions
and metadata apart from disclosed helpers. Only owned regression code executes.
