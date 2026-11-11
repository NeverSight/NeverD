package fixture
open class Base<U>(@JvmField val start: Float, @JvmField val end: Float, @JvmField val data: U?) {
    init {
        Trace.base = this
        Trace.step(1)
        Trace.probe.accept(this)
        Trace.step(2)
    }
}
