Genuine Kotlin upcast of the original self value before closed callback preparation. The class/interface intersection and callback tail preserve original late checks, getter/class initialization order, nulls, callback identity and exact failure behavior.

The runner's `--branch-entry` variant copies the authored inputs into a fresh
directory and adds a conditional event before that same upcast. It verifies that
the unmodified Kotlin compiler and D8 emit a branch to the argument copy, followed
by the original cast and callback getter. Frozen callers alternate both paths and
observe failures before the copy as well as the existing preparation, class
initialization and callback failures. The same-code wrong-bound input must still
fail at its original later cast and be rejected by recovery. Native checks reject
entries after the copy, incompatible incoming states and protected regions.
