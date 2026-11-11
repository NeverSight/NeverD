package fixture

interface Scope {
    suspend fun <T> runWindow(tick: Long, block: suspend Scope.() -> T): T
}

class Core(@JvmField val token: Any?) {
    private inner class Session<R> : Scope {
        override suspend fun <T> runWindow(tick: Long, block: suspend Scope.() -> T): T {
            Trace.captureWindow(this, this@Core, token, tick)
            return block()
        }
    }
    fun openSession(): Any = Session<Any?>()
    suspend fun <T> relay(session: Any, tick: Long, block: suspend Scope.() -> T): T {
        Trace.step(1)
        return (session as Session<*>).runWindow(tick, block)
    }
}
