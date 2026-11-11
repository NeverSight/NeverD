package fixture
interface Parent {
    fun matches(predicate: (Parent) -> Boolean): Boolean {
        Trace.enter(this, predicate)
        return predicate(this)
    }
}
