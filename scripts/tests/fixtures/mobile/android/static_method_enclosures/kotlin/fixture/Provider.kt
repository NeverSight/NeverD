package fixture

object Provider {
    init { Trace.event(9) }

    @JvmStatic @JvmSynthetic @JvmName("read-value")
    fun read(value: Any?, marker: Any? = object {}): Any? {
        Trace.marker = marker
        Trace.event(1)
        val result = Trace.apply(value)
        Trace.result = result
        Trace.event(3)
        return result
    }

    @JvmStatic fun relay(value: Any?): Any? = read(value)
    @JvmStatic fun provided(value: Any?, marker: Any?): Any? = read(value, marker)
}
