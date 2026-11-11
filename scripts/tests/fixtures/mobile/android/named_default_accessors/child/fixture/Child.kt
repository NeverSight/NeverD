package fixture
interface Child : Parent
class Plain : Child
class Receiver : Child {
    override fun `draw-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(this,2,token,a,b,c,d,scale,mode)
    }
}
class Invoker {
    fun through(receiver: Child?,token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        receiver?.`draw-slot`(token,a,b,c,d,scale,mode)
    }
}
