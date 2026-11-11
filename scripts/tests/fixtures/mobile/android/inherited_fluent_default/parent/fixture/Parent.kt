package fixture
interface Parent {
    fun combine(other: Parent?): Parent? = Trace.choose(this, other) as Parent?
}
