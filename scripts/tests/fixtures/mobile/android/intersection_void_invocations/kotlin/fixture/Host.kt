package fixture
class Host : First, Second {
    override fun first() { Trace.firstCalls++; Trace.hit(4) }
    override fun second() { Trace.secondCalls++; Trace.hit(5) }
    fun run() { Provider.enable(this) }
}
