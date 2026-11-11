package fixture
object Entry {
    init { Trace.hit(5); Trace.values[0] = -19 }
    @JvmStatic @JvmName("carry-value")
    fun <T> carry(value: Int): Int {
        Trace.hit(4)
        val result = Provider.relay<T>(value)
        Trace.after = result
        return result
    }
    @JvmStatic fun call(value: Int): Int = carry<Any?>(value)
    @JvmStatic fun __neverdStaticMethod0(): Int = 17
}
