package fixture
class Gate<T> {
    @JvmField var payload: Payload? = null
    @JvmField var count: Int = 0
    @JvmField var failure: RuntimeException? = null
    fun update(lock: Any?, callback: Runnable?) {
        synchronized(lock!!) {
            if (payload == null) payload = Payload(lock)
            if (callback != null) callback.run()
            val previous = count
            count = previous + 1
            if (failure != null) throw failure!!
        }
    }
}
