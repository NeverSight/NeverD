package fixture
import java.util.concurrent.atomic.AtomicInteger
class Counter(initial: Int) : AtomicInteger(initial) {
    override fun toByte(): Byte = toInt().toByte()
    override fun toShort(): Short = toInt().toShort()
}
