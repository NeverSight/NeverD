package fixture

class Core @JvmOverloads constructor(@JvmField val value: Int = Defaults.provider.read()) : Base(value) {
    init {
        Trace.core = this
        Trace.hit(5)
    }
}
