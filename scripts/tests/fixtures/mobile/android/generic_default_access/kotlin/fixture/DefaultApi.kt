package fixture
interface DefaultApi {
    fun current(): String?
    fun <V : Bound> work(link: Link<String, V>?): Vector<V> {
        Trace.hit(1)
        Trace.link = link
        val input = current()
        Trace.input = input
        val value = link!!.apply(input)
        Trace.hit(4)
        val result = Vector(this, value, Trace.wide)
        Trace.result = result
        Trace.hit(7)
        return result
    }
}
open class DefaultBase(@JvmField var currentValue: String?) : DefaultApi {
    override fun current(): String? {
        Trace.hit(2)
        return currentValue
    }
}
class DefaultOverride(currentValue: String?) : DefaultBase(currentValue) {
    override fun <V : Bound> work(link: Link<String, V>?): Vector<V> {
        Trace.overrides++
        Trace.hit(9)
        @Suppress("UNCHECKED_CAST")
        return Trace.overrideResult as Vector<V>
    }
}
