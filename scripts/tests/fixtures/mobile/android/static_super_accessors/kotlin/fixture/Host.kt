package fixture
open class Host : Root() {
    internal inline fun callBase() { super.ping() }
    override fun ping() { Trace.overrides++; Trace.hit(4) }
}
