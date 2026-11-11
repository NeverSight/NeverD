# Captured closed constructor delegation

Independently authored Java reference with a closed generic callback and a
captured outer instance. A private primary constructor receives the callback;
the public wide constructor delegates before publishing its wide field. The
source Signature omits the implicit outer parameter. A private/public member
variant and source/SDK callback declarations share frozen direct, reflection
and MethodHandle observers. No third-party binary or application data is used.

The observer checks all partial fields and callback results, class initialization,
null and incompatible failure routes, null outer values in reflective calls,
raw wide bits, capture identity, and the existing explicit capture metadata
projection. Six independent wrong programs must fail that observer.

The synthetic-access profile authors a raw public forwarding constructor with
one unused Object marker and a private wide target. Its input encoding changes
only the raw constructor's synthetic access bit. Every Code byte, descriptor
and other attribute stays exact; original and encoded classes both execute
against the same frozen observer before recovery. Rebuilt source discloses the
synthetic flag difference through the existing member modifier receipt. A
seventh wrong program incorrectly forwards the unused marker as the callback.
