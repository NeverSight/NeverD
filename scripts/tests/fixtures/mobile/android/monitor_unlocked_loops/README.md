# Ordinary loops outside a monitor

The Java reference captures two counters before a leading loop, one synchronized
region, and a trailing loop. The independently authored smali has the same
original fields and methods, with split catch-all cleanup inside the monitor.
Neither ordinary loop acquires the lock. No compiler classfile or DEX is edited.

The frozen JVM observer exercises direct, separate caller, reflection and method
handle routes. It checks callback order and lock state, captured counter values
despite later mutations, partial allocation and count updates, original
Exception/Error identity, null acquisition after the leading loop, null
callbacks, and reentrant calls from an unlocked callback. The shared monitor
observers also check 6000 concurrent updates and exact releases. Eleven wrong
Java programs and three unacquired control-flow entries must be detected.
Every generated Java file compiles at release 8; all original definitions,
class/method metadata and Exceptions are compared in source and SDK modes.
