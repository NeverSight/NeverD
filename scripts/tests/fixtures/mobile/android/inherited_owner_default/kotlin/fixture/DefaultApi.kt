package fixture
interface DefaultApi<V : Vector> {
    fun duration(first: V?, second: V?, third: V?): Long
    fun velocity(time: Long, first: V?, second: V?, third: V?): V?
    fun finish(first: V?, second: V?, third: V?): V? {
        Trace.first = first
        Trace.second = second
        Trace.third = third
        Trace.hit(1)
        val time = duration(first, second, third)
        Trace.hit(3)
        val result = velocity(time, first, second, third)
        Trace.hit(5)
        return result
    }
}
interface ChildApi<V : Vector> : DefaultApi<V>
interface GrandApi<V : Vector> : ChildApi<V>
open class DefaultBase<V : Vector> : GrandApi<V> {
    override fun duration(first: V?, second: V?, third: V?): Long {
        Trace.hit(2)
        return Trace.wide
    }
    override fun velocity(time: Long, first: V?, second: V?, third: V?): V? {
        Trace.received = time
        Trace.hit(4)
        return third
    }
}
class DefaultOverride<V : Vector> : DefaultBase<V>() {
    override fun finish(first: V?, second: V?, third: V?): V? {
        Trace.overrides++
        Trace.hit(9)
        return second
    }
}
