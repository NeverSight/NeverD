package fixture
class Core(
    @JvmField val first: Any? = null,
    @JvmField val second: Any? = null,
    @JvmField val rest: Array<out Any?>? = null,
    @JvmField val handler: Handler?
) {
    @JvmField var legacyBlock: (suspend Context.() -> Unit)? = null
    init {
        Trace.partial=this
        Trace.step(1)
        Trace.capture(first, second, rest, handler)
        Trace.step(3)
    }
    constructor(first: Any?, second: Any?, rest: Array<out Any?>?, block: (suspend Context.() -> Unit)?) : this(first, second, rest, Provider.select()) {
        Trace.legacy=this
        legacyBlock=block
        Trace.step(5)
    }
}
