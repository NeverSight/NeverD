package fixture
class Core @JvmOverloads constructor(token: Any, count: Int = Trace.count(),
                                    wide: Long = Trace.wide(), number: Double = Trace.number())
    : Base(token, count, wide, number) {
    @JvmField val stamp: Int = Trace.event(5)
}
