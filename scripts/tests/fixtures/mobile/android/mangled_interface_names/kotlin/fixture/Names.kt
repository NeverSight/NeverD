package fixture

@JvmInline value class Token(val value: Any?)

interface Api<T> { fun send(value: T): Token }
interface Branch<T> : Api<T>

open class Relay<T>(val target: Api<T>) : Branch<T> {
    override fun send(value: T): Token {
        Trace.hit(2)
        return target.send(value)
    }
}

class Child<T>(target: Api<T>) : Relay<T>(target) {
    override fun send(value: T): Token {
        Trace.hit(1)
        return super.send(value)
    }
}

fun call(api: Api<Any?>, value: Any?): Any? = api.send(value).value
fun inherited(api: Branch<Any?>, value: Any?): Any? = api.send(value).value
fun concrete(relay: Relay<Any?>, value: Any?): Any? = relay.send(value).value
fun __neverdVirtualMethod0(): Int = 17

class Terminal(val target: Api<Any?>) {
    fun send(value: Any?): Token {
        Trace.hit(4)
        return target.send(value)
    }
}

fun closed(terminal: Terminal, value: Any?): Any? = terminal.send(value).value
