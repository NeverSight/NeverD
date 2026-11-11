package fixture
abstract class Foundation(@JvmField val seed: Any?) {
    abstract fun before(bits: Int)
    abstract fun after()
}
abstract class Root(seed: Any?): Foundation(seed) {
    @JvmSynthetic
    @Suppress("UNCHECKED_CAST")
    inline fun <T> `walk-stage`(bits: Int, noinline action: (T) -> Unit) {
        if (bits == 0) return
        before(bits)
        action(seed as T)
        after()
    }
}
open class Child(seed: Any?): Root(seed) {
    override fun before(bits: Int) {}
    override fun after() {}
}
fun sendRoot(root: Root, bits: Int, action: (Any?) -> Unit) {
    root.`walk-stage`(bits, action)
}
fun sendChild(child: Child, bits: Int, action: (Any?) -> Unit) {
    child.`walk-stage`(bits, action)
}
