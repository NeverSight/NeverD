The independent Java oracle and authored smali put acquisition and post-release
work inside an outer catch-all. A locked failure restores a captured reference
and count, then throws through the original release into unlocked nested finally
actions. Callbacks mutate the source field after the reference was captured.

Frozen direct, compiled, reflective and method-handle callers check 1280 cleanup
failure masks, 128 null acquisitions, eight normal/post-release routes, nested
calls and the existing concurrency oracle. Every cleanup observes the expected
lock state, original or replacement exception object, and partial field state.
