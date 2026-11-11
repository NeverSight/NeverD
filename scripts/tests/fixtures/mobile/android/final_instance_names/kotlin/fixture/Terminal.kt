package fixture
class Terminal : Base() {
    @JvmSynthetic
    @JvmName("read-value")
    fun read(input: Any?): Any? {
        Trace.hit(1)
        return Trace.consume(input)
    }
    fun entry(input: Any?): Any? = read(input)
}
