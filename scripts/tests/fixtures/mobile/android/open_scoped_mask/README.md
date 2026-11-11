Owned Kotlin witness for a public open generic owner and an original raw
synthetic default constructor. Its only default replaces the owner-formal
payload with null; two float inputs retain their exact bits. Constructor-body
observers expose field initialization, object identity and partial failures.

The runner compiles the unmodified Kotlin source and uses genuine D8 output.
Frozen callers compare reflection and MethodHandle routes on original and
recompiled programs, while metadata accounting discloses only the synthetic
constructor flag and independently verifies D8's CLASS annotation loss.
Source/SDK profiles, same-Code Signature controls and separately compiled
wrong programs challenge the proof and its observer.
