package fixture;
public class Value {
 public final Object input;
 public final long tag;
 public Value(Object input,long tag) throws java.io.IOException {
  Trace.hit(1);
  this.input = input;
  this.tag = tag;
  Trace.allocated = this;
  Trace.valueAction.run();
  Trace.hit(3);
 }
}
