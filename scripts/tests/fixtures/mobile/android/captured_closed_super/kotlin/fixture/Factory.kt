package fixture

interface Scope

@JvmInline
value class Factory<T>(val holder: java.util.concurrent.atomic.AtomicReference<T>) {
    fun <R> make(seed: (Scope) -> T, operation: suspend (T) -> R): suspend Scope.() -> R =
        { val value = seed(this); holder.set(value); operation(value) }
}
