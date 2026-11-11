package fixture

open class Span<T>(@JvmField val start: Float, @JvmField val end: Float,
                   @JvmField val data: T? = null) {
    init {
        Trace.partial = this
        Trace.step(1)
        Trace.seen = data
        Trace.step(2)
    }
}
