package fixture;
public class Core {
 public final Value value;
 public long stamp;
 public Core(Object input,long tag,boolean after) {
  Trace.partial = this;
  this.stamp = tag;
  try {
   this.value = new Value(input, tag);
   Trace.hit(4);
   if (after) Trace.afterAction.run();
  } catch (java.io.IOException failure) {
   Trace.caught = failure;
   Trace.hit(6);
   throw new Wrapped(failure);
  }
  Trace.hit(8);
 }
}
