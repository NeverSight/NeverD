@file:JvmName("Owner")
package fixture
fun <V : Narrow> create(seed: V?, first: Float, second: Float): Api {
    Trace.seed=seed;Trace.first=first;Trace.second=second;Trace.hit(1)
    return object : Api {
        private val values=arrayOf<Narrow?>(seed,null)
        override fun get(index: Int): Narrow? { Trace.hit(2);return values[index] }
    }
}
