@file:JvmName("ScopeFacade")
@file:JvmMultifileClass
package fixture

// Generates the owned package-part scope witness, independently of Java bodies.
@JvmSynthetic
fun <E> Target<E>.register(element: E): Unit {}
