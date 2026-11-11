package fixture

interface Parent {
    fun find(value: Any?): Any? = Trace.parent(this, value)
}
