package fixture

private class Core<T>(@JvmField val seed:T) {
 fun <R> convert(mapper:(T)->R,limit:Int=Int.MIN_VALUE):Box<R> {
  Trace.record(0,this,mapper,seed,limit,null)
  val item=mapper(seed)
  val result=Box(item,limit)
  Trace.record(1,this,mapper,seed,limit,result)
  return result
 }
}
