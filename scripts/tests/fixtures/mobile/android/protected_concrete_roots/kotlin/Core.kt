package fixture
abstract class Parent {
    var stored: Long = 0
        private set
    protected open fun `write-value`(value: Long) {
        Trace.before(this,value,1)
        stored=value
        Trace.after()
    }
    fun send(value: Long) { `write-value`(value) }
}
class PlainChild: Parent()
class OverrideChild: Parent() {
    var chosen: Long = 0
        private set
    protected override fun `write-value`(value: Long) {
        Trace.before(this,value,2)
        chosen=value
        Trace.after()
    }
}
abstract class PublicPeer {
    var stored: Long = 0
        private set
    open fun `write-value`(value: Long) {
        Trace.before(this,value,3)
        stored=value
        Trace.after()
    }
    fun send(value: Long) { `write-value`(value) }
}
class PlainPublic: PublicPeer()
class PublicChild: PublicPeer() {
    var chosen: Long = 0
        private set
    override fun `write-value`(value: Long) {
        Trace.before(this,value,4)
        chosen=value
        Trace.after()
    }
}
