package fixture
class Gate<T> {
    @JvmField var payload: Payload? = null
    @JvmField var count: Int = 0
    @JvmField var failure: RuntimeException? = null
    fun update(lock: Any?, callback: Runnable?, after: Runnable?, enabled: Boolean): Int {
        val selected = synchronized(lock!!) {
            if (payload == null) payload = Payload(lock)
            if (callback != null) callback.run()
            count = count + 1
            if (failure != null) throw failure!!
            enabled
        }
        if (selected && after != null) after.run()
        return count
    }
}
