package fixture
class Core<T>(@JvmField val count: Int, @JvmField val capacity: Int = 7,
              @JvmField val callback: Sink<in T> = Defaults.CB) {
    init { Trace.partial = this; Trace.hit(4) }
}
