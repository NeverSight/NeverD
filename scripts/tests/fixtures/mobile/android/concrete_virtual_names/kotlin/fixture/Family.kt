package fixture

abstract class Root : InitBase() {
    @Suppress("INAPPLICABLE_JVM_NAME")
    @JvmName("read-value")
    open fun read(input: Any?): Any? {
        Trace.hit(1)
        return Trace.consume(input)
    }
    fun entry(input: Any?): Any? = read(input)
}
open class Middle : Root()
open class Child : Middle() {
    @Suppress("INAPPLICABLE_JVM_NAME")
    @JvmName("read-value")
    override fun read(input: Any?): Any? {
        Trace.hit(2)
        return super.read(input)
    }
    fun parent(input: Any?): Any? = super.read(input)
}
class Leaf : Child() {
    @Suppress("INAPPLICABLE_JVM_NAME")
    @JvmName("read-value")
    override fun read(input: Any?): Any? {
        Trace.hit(4)
        return super.read(input)
    }
}
