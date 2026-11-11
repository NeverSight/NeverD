package fixture
class Core {
    @JvmField val value: Api
    @JvmField val label: String?
    constructor(value: Api, label: String?) {
        Trace.partial=this;Trace.hit(3)
        this.value=value;Trace.hit(4)
        this.label=label;Trace.hit(5)
    }
    constructor(input: Any?, word: Long, label: String?) : this(object : Api {
        private val captured: Any?
        private val bits: Long
        init {Trace.hit(1);captured=input;bits=word;Trace.input=input;Trace.hit(2)}
        override fun read(): Any? {Trace.local=this;Trace.word=bits;return captured}
    }, label)
}
