package fixture
interface Api {
    @JvmSynthetic fun `write-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int)
    @JvmSynthetic fun `default-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(7,token,a,b,c,d,scale,mode)
    }
}
interface Branch : Api
class Alpha : Api {
    @JvmSynthetic override fun `write-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(1,token,a,b,c,d,scale,mode)
    }
}
class Beta : Branch {
    @JvmSynthetic override fun `write-slot`(token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) {
        Trace.accept(2,token,a,b,c,d,scale,mode)
    }
}
class Invoker {
    fun through(api: Api?,token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) { api?.`write-slot`(token,a,b,c,d,scale,mode) }
    fun branch(api: Branch?,token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) { api?.`write-slot`(token,a,b,c,d,scale,mode) }
    fun concrete(api: Alpha?,token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) { api?.`write-slot`(token,a,b,c,d,scale,mode) }
    fun defaults(api: Api?,token: Any?,a: Long,b: Long,c: Long,d: Long,scale: Float,mode: Int) { api?.`default-slot`(token,a,b,c,d,scale,mode) }
}
