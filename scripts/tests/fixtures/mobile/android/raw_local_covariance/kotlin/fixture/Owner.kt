package fixture
class Owner<K : Any>(@JvmField val callback: (K) -> Any? = { Trace.hit(3); Trace.seen = it; null }) {
    init { Trace.partial = this; Trace.hit(2) }
}
