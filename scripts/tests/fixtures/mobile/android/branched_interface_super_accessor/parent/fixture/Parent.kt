package fixture
interface Parent : Root {
    fun measure(value: Long): Int {
        Trace.begin(this, value)
        val result = read(value)
        Trace.after(result)
        return result
    }
}
