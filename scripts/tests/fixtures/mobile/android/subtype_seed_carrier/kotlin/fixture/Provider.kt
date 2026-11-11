@file:JvmName("Provider")
package fixture

@Suppress("UNCHECKED_CAST")
fun <T : R, R> select(source: Producer<out T>?, initial: R,
                     context: Context?, flags: Long, bits: Int): Result<R>? {
    val captured = source
    Trace.seenProducer = captured
    Trace.seenSeed = initial
    Trace.seenFlags = flags
    Trace.seenBits = bits
    Trace.mask = Trace.mask or 1
    Trace.hit(1)
    val value: T = captured!!.value()
    Trace.seenValue = value
    Trace.mask = Trace.mask or 2
    Trace.hit(2)
    context!!.accept(value, initial, flags, bits)
    Trace.mask = Trace.mask or 4
    Trace.hit(3)
    val result = Trace.output as Result<R>?
    Trace.seenResult = result
    Trace.mask = Trace.mask or 8
    Trace.hit(4)
    return result
}
