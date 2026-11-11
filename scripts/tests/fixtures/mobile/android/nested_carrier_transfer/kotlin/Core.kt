package fixture

class Box<out T>(@JvmField val item:T)
class Caller {
 fun <T> call(input:List<Box<T>>?,first:Int,second:Int):List<Box<T>>? = transfer(input,first,second)
}
@Suppress("UNCHECKED_CAST")
private fun <T> transfer(input:List<Box<T>>?,first:Int,second:Int):List<Box<T>>? {
 Trace.record(0,input,first,second,null)
 val result=Trace.choose(input,first,second) as List<Box<T>>?
 Trace.record(1,input,first,second,result)
 return result
}
