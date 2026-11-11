package fixture
@JvmInline value class Bits(val raw: Long)
interface Api { fun make(bits: Bits): Value? }
object Factory {
    @JvmField val shared: Api = object : Api {
        override fun make(bits: Bits): Narrow? {
            Trace.hit(1); Trace.receiver = this; Trace.calls++; Trace.wide = bits.raw
            Trace.hit(2); return Trace.reply
        }
    }
}
class Core {
    fun call(bits: Long): Value? {
        Trace.hit(3)
        val result = Factory.shared.make(Bits(bits))
        Trace.hit(4)
        return result
    }
}
