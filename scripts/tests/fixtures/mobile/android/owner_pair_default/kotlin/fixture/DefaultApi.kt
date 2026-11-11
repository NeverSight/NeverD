package fixture
interface DefaultApi<S> {
    fun initial(): S
    fun target(): S
    fun same(first: S, second: S): Boolean {
        Trace.first = first
        Trace.second = second
        Trace.hit(1)
        val a = initial()
        Trace.hit(3)
        if (first != a) return false
        val b = target()
        Trace.hit(5)
        return second == b
    }
}
open class DefaultBase<S>(@JvmField var firstValue: S, @JvmField var secondValue: S) : DefaultApi<S> {
    override fun initial(): S { Trace.hit(2); return firstValue }
    override fun target(): S { Trace.hit(4); return secondValue }
}
class DefaultOverride<S>(firstValue: S, secondValue: S) : DefaultBase<S>(firstValue,secondValue) {
    override fun same(first: S,second: S): Boolean { Trace.overrides++;Trace.hit(9);return true }
}
