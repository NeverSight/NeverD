# Captured exceptions cross shared monitor cleanup

This independently authored Gate uses the established immutable Java reference,
Payload and concurrency observer. Three catch-all landing pads copy their captured
exception into another register. The last handler lies after the shared release
and jumps backward to it. Its protected interval also covers the exact release.
Only the original must-alias monitor proof can treat that release as total.

The input smali is not a rewritten compiler original and is not executed as a JVM
class. Recompiled recovery is checked against frozen source behavior, metadata,
ordered effects, null locks, reentrancy, partial construction, checked/Error
identity and actual release to a competing thread. Existing wrong programs must
continue to fail the observer. Native CFG tests cover copy forms, register reuse,
handler ordering, backward joins and invalid acquisition/release boundaries.
