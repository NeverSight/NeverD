package fixture
interface Parent {
    fun <R> foldIn(seed: R, operation: (R, Parent) -> R): R {
        Trace.enter(this, seed, operation)
        return operation(seed, this)
    }
    fun <R> foldOut(seed: R, operation: (Parent, R) -> R): R {
        Trace.enter(this, seed, operation)
        return operation(this, seed)
    }
}
