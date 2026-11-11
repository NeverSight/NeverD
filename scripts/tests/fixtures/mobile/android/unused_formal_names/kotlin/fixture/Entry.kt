package fixture
object Entry {
    init { Trace.hit(5) }
    @JvmStatic @JvmName("carry-value")
    fun <T> carry(value: Any?): Any? {
        Trace.hit(4)
        val result = Provider.relay<T>(value)
        Trace.after = result
        return result
    }
    @JvmStatic fun call(value: Any?): Any? = carry<Any?>(value)
    @JvmStatic fun __neverdStaticMethod0(): Int = 17
}
