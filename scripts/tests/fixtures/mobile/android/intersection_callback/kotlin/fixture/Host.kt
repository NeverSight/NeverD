package fixture
class Host(@JvmField val callback: Callback<Unit>?) : Base(), Observer {
    override fun observe() { Trace.nodeCalls++; Trace.hit(4) }
    fun fetch(): Callback<Unit>? { Trace.getterCalls++; Trace.hit(5); return callback }
    fun run() { Provider.observe(this, fetch()) }
}
