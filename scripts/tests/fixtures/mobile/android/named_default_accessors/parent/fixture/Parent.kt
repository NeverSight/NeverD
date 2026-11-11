package fixture
interface Parent {
    fun `draw-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(this,7,token,a,b,c,d,scale,mode)
    }
}
