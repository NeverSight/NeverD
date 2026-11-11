package fixture

@Suppress("UNCHECKED_CAST")
class Core<T : Base> private constructor(value: T, token: Any? = null) : Parent(), Marker {
    @JvmField val value: T = Trace.value(this, value) as T
    @JvmField val token: Any? = Trace.token(this, token)
    init { Trace.hit(4, this) }
    constructor(producer: Producer<out T>) : this(producer.produce())
}
