@file:JvmName("MetadataHost")
package fixture

// This owned declaration generates the frozen metadata witness. Its bytecode
// is not the behavior oracle; the Java fixture independently supplies bodies.
@JvmSynthetic
fun <X, Y> adapt(input: Box<X>, action: Callback<X, Box<Y>>): Box<Y> = Box<Y>()
