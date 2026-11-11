package fixture
interface DefaultApi<N> {
    fun current(): N
    fun work(block: ((N, Any?) -> Unit)?, value: Any?) {
        Trace.hit(1)
        val input = current()
        Trace.input = input
        Trace.hit(3)
        block!!(input, value)
        Trace.hit(5)
    }
}
open class DefaultBase(@JvmField var currentValue: Any?) : DefaultApi<Any?> {
    override fun current(): Any? {
        Trace.receiver = this
        Trace.hit(2)
        return currentValue
    }
}
class DefaultOverride(currentValue: Any?) : DefaultBase(currentValue) {
    override fun work(block: ((Any?, Any?) -> Unit)?, value: Any?) {
        Trace.overrides++
        Trace.hit(9)
    }
}
