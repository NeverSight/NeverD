package fixture
interface Scope : Context {
    suspend fun <T> runWindow(tick: Long, block: suspend Scope.() -> T): T {
        Trace.captureWindow(this, token(), tick)
        return block()
    }
    suspend fun <T> runGauge(tick: Double, block: suspend Scope.() -> T): T {
        Trace.captureGauge(this, token(), tick)
        return block()
    }
}
