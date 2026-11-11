package fixture
class Outer {
    inner open class Host : Root() {
        internal inline fun callBase() { super.ping() }
        override fun ping() { Trace.overrides++; Trace.hit(4) }
    }
}
