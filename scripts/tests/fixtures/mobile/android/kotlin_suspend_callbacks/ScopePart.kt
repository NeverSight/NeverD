@file:JvmName("ScopeFacade")
@file:JvmMultifileClass
package fixture

import kotlin.coroutines.CoroutineContext

// Generates the owned package-part scope witness, independently of Java bodies.
@JvmSynthetic
fun <E> Target<E>.repeat(count: Int, context: CoroutineContext, element: E, action: suspend (E) -> E): Target<E> = this
