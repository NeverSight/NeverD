package fixture
interface DefaultApi<T,V : Bound> {
    fun current(): Any?
    fun work(value: Any?, tag: String?): Any? {
        Trace.hit(1)
        Trace.tag = tag
        val callback = object : Callback {
            override fun run(): Any? {
                Trace.hit(5)
                return value
            }
        }
        Trace.captured = callback
        Trace.hit(2)
        Trace.current = current()
        Trace.hit(3)
        val result = callback.run()
        Trace.returned = result
        Trace.hit(4)
        return result
    }
}
open class DefaultBase(@JvmField var currentValue: Any?) : DefaultApi<Any?,Bound> {
    override fun current(): Any? {
        Trace.hit(6)
        return currentValue
    }
}
class DefaultOverride(currentValue: Any?) : DefaultBase(currentValue) {
    override fun work(value: Any?, tag: String?): Any? {
        Trace.overrides++
        Trace.hit(9)
        return Trace.overrideResult
    }
}
