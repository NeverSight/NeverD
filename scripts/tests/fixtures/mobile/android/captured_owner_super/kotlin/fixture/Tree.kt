package fixture
class Tree<T,U> {
    @JvmField var seed: T? = null
    inner class Node(start: Float, end: Float, data: T?, @JvmField val color: Int) : Base<T>(start,end,data) {
        @JvmField val min: Float = start
        @JvmField val max: Float = end
        init {
            Trace.child = this
            Trace.step(3)
        }
    }
}
