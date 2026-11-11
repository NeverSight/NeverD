@file:JvmName("Callbacks")
package fixture
inline fun <K : Any> make(noinline callback: (K) -> Any? = { Trace.hit(3); Trace.seen = it; null }) {
    Trace.callback = callback
    val stage = object : Runnable { override fun run() { Trace.hit(2) } }
    stage.run()
}
