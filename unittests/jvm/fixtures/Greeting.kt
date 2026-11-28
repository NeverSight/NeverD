package example

class Greeting(private val base: Int) {
    fun count(extra: Int): Int = base + extra
}
