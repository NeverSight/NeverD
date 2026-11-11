package fixture
class Core(@JvmField val image: Shape, @JvmField val offset: Long=Trace.offset(),
           @JvmField val size: Long=Trace.record((image.width().toLong() shl 32) or (image.height().toLong() and 0xffffffffL))) {
    init {
        Trace.partial=this
        Trace.step(5)
        Trace.imageSeen=image
        Trace.offsetSeen=offset
        Trace.sizeSeen=size
        Trace.step(6)
    }
}
class Invoker {
    fun make(image: Shape,offset: Long,size: Long,mask: Int): Core = when(mask and 6) {
        0 -> Core(image,offset,size)
        2 -> Core(image,size=size)
        4 -> Core(image,offset=offset)
        else -> Core(image)
    }
}
