package fixture
class Core(@JvmField val number:Float,
           @JvmField val range:Range<Float>?,
           @JvmField val count:Int=7) {
 init { Trace.record(this,number,range,count) }
}
