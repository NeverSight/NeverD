package fixture

object Provider {
    init { Trace.event(1) }

    @JvmStatic @JvmSynthetic @JvmName("read-value")
    fun read(value: Any?): Any? {
        Trace.event(2)
        return value
    }

    @JvmStatic @JvmSynthetic @JvmName("touch-value")
    fun touch(value: Any?) { Trace.event(3) }

    @JvmStatic @JvmSynthetic @JvmName("read-value")
    fun wide(value: Long): Long {
        Trace.event(4)
        return value
    }

    @JvmStatic fun relay(value: Any?): Any? = read(value)
    @JvmStatic fun effect(value: Any?): Any? { touch(value); return read(value) }
    @JvmStatic fun allocate(value: Any?): Box = Box(read(value))
    @JvmStatic fun wideRelay(value: Long): Long = wide(value)
}
