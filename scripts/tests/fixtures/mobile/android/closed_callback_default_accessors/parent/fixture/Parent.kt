package fixture
interface Parent {
    fun `draw-slot`(token: java.util.function.Consumer<in Parent>?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(this,7,token,a,b,c,d,scale,mode)
        token?.accept(this)
    }
}
