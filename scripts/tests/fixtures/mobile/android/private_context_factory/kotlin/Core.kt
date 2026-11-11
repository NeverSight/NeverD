package fixture

interface Parent<A,B> {
 fun save(context:Context,value:A):B
 fun restore(value:B):A
}
interface Pair<A,B>:Parent<A,B>
private class Impl<A,B>(private val saveFn:(Context,A)->B,private val restoreFn:(B)->A):Pair<A,B> {
 override fun save(context:Context,value:A):B {
  Trace.operation(2,this,context,value,null)
  val result=saveFn(context,value)
  Trace.operation(3,this,context,value,result)
  return result
 }
 override fun restore(value:B):A {
  Trace.operation(4,this,null,value,null)
  val result=restoreFn(value)
  Trace.operation(5,this,null,value,result)
  return result
 }
}
private fun <Original,Stored> make(saveFn:(Context,Original)->Stored,restoreFn:(Stored)->Original):Pair<Original,Stored> {
 Trace.construct(0,saveFn,restoreFn,null)
 val result=Impl(saveFn,restoreFn)
 Trace.construct(1,saveFn,restoreFn,result)
 return result
}
fun call(saveFn:(Context,Any?)->Any?,restoreFn:(Any?)->Any?):Pair<Any?,Any?> = make(saveFn,restoreFn)
