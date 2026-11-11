package fixture
@Suppress("UNCHECKED_CAST")
fun <T> build(first:String?,second:String?,merge:(T,T)->T):Box<T> {
 Trace.record(first,second,merge)
 return Trace.output as Box<T>
}
fun call(first:String?,second:String?,merge:(Any?,Any?)->Any?):Box<Any?> =
 build(first,second,merge)
