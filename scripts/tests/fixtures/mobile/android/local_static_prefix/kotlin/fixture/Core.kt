package fixture
class Core<T>(@JvmField val count: Int, @JvmField val capacity: Int=7,
              @JvmField val first: (Any?)->Any?={Trace.accept(1,it)},
              @JvmField val second: (Any?)->Any?={Trace.accept(2,it)}) {
    init {
        Trace.partial=this
        Trace.step(3)
        Trace.firstSeen=first
        Trace.secondSeen=second
        Trace.step(4)
    }
}
class Invoker {
    fun make(count: Int): Core<Any?> = Core(count)
}
