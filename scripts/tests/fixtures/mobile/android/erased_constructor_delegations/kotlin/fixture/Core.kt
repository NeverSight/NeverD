package fixture

class Core private constructor(val first: Int, val second: Int, val bits: Long,
                               val callback: (Any) -> Boolean) {
    val answer: Boolean
    init {
        Trace.step(2)
        Trace.seen = this
        answer = callback(this)
        Trace.step(3)
        if (Trace.failBody) throw Trace.failure
    }
    class Factory {
        fun make(first: Int, second: Int, bits: Long, callback: (Any) -> Boolean): Core =
            Core(first, second, bits, callback)
    }
}
