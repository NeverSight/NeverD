package fixture
class Core {
    fun make(wide: Long, token: Any?, hook: Hook?): java.util.function.LongSupplier =
        java.util.function.LongSupplier { Trace.read(wide, token, hook) }
}
