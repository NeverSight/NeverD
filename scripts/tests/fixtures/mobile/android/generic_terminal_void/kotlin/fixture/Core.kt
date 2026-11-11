package fixture

class Key<T>
abstract class Base {
    abstract fun <T> writeValue(key: Key<T>, value: T)
}
object Core : Base() {
    override fun <T> writeValue(key: Key<T>, value: T): Nothing = error("owned terminal message")
}
