package fixture
@JvmInline value class Scalar(val raw: Float) : Comparable<Scalar> {
    override fun compareTo(other: Scalar): Int { Trace.hit(2);return raw.compareTo(other.raw) }
}
fun boxed(value: Float): Any { Trace.hit(1);return Scalar(value) }
