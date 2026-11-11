package fixture

class Core<T>(input:T,first:Int,second:Int,label:String) {
 init {Trace.record(0,this,input,first,second,label)}
 @JvmField val item:T=input
 @JvmField val start:Int=first
 @JvmField val end:Int=second
 @JvmField val tag:String=label
 init {Trace.record(1,this,input,first,second,label)}
 constructor(input:T,first:Int,second:Int):this(input,first,second,"") {
  Trace.record(2,this,input,first,second,"")
 }
}
