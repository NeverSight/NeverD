package fixture

open class Provider<T>(private var seed: Long) {
    @JvmName("combine-value")
    private fun combine(first: Long, second: Long): Long {
        Trace.hit(1)
        seed += first
        Trace.hit(2)
        val result = seed + second
        Trace.result = result
        Trace.hit(3)
        return result
    }

    fun call(first: Long, second: Long): Long = combine(first, second)
    fun __neverdPrivateMethod0() {}
}

class Child(seed: Long): Provider<Any>(seed) {
    @JvmName("combine-value")
    fun own(first: Long, second: Long): Long {
        Trace.hit(4)
        return first - second
    }
    fun other(first: Long, second: Long): Long = own(first, second)
}
