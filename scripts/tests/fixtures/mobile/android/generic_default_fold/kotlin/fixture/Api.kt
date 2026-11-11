package fixture

interface Api {
    fun <R> fold(seed: R, operation: Bi<in R, in Api, out R>): R {
        Trace.before(this, seed)
        val result = operation.invoke(seed, this)
        Trace.after(result)
        return result
    }
}
