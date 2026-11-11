package fixture
class Key<T>(val value: T)
class Box<T>(val value: T)
interface Reader { fun <T> get(key: Key<T>): T }
class Core: java.util.HashMap<Key<Any?>,Box<Any?>>(), Reader {
    @Suppress("UNCHECKED_CAST")
    override fun <T> get(key: Key<T>): T {
        Trace.hits++
        Trace.seen = key
        if (Trace.failure != null) throw Trace.failure
        return Trace.result as T
    }
}
