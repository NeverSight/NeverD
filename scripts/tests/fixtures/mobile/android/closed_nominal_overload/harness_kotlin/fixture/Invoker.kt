package fixture
class Invoker {
    fun make(first: Any?, second: Any?, rest: Array<out Any?>?, handler: Handler?, mask: Int): Core = when (mask and 7) {
        0 -> Core(first, second, rest, handler=handler)
        1 -> Core(second=second, rest=rest, handler=handler)
        2 -> Core(first=first, rest=rest, handler=handler)
        3 -> Core(rest=rest, handler=handler)
        4 -> Core(first=first, second=second, handler=handler)
        5 -> Core(second=second, handler=handler)
        6 -> Core(first=first, handler=handler)
        else -> Core(handler=handler)
    }
}
