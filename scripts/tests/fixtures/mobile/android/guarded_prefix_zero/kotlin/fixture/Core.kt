package fixture
class Core(input: Any?, alternate: Boolean) :
 Parent(Trace.seed(if (alternate) 17 else 29), null) {
 init {
  val child = input as? Child
  if (child != null) Trace.accept(this, child)
  Trace.finish(this)
 }
}
