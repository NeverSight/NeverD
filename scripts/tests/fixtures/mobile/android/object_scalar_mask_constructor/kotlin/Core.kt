package fixture

private class Core<T>(input:T,start:Int,limit:Int=Int.MIN_VALUE,label:String="") {
 init {Trace.record(0,this,input,start,limit,label)}
 val value:T=input
 val first:Int=start
 val second:Int=limit
 val tag:String=label
 init {Trace.record(1,this,input,start,limit,label)}
}
