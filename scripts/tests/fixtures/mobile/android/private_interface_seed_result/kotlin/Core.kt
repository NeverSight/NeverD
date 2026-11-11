package fixture
class Core {
    @JvmField var calls: Int = 0
    @Suppress("UNCHECKED_CAST")
    private fun <T: CharSequence> select(value: T?, size: Int): T? {
        calls++
        require(size > 0)
        if (value == null || value.length <= size) return value
        return value.subSequence(0,size) as T
    }
    fun send(value: CharSequence?, size: Int): CharSequence? = select(value,size)
    fun typed(value: Text?, size: Int): Text? = select(value,size)
    fun peer(other: Core?, value: CharSequence?, size: Int): CharSequence? = other!!.select(value,size)
}
