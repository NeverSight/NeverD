@file:JvmName("Callbacks")
package fixture
inline fun <K : Any> make(noinline callback: (Int, K) -> Any? = { index, value -> Trace.hit(3); Trace.index = index; Trace.seen = value; null }) {
    Trace.callback = callback
    val stage = object : Runnable { override fun run() { Trace.hit(2) } }
    stage.run()
}
