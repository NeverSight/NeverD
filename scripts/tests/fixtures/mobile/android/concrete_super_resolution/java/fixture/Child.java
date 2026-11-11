package fixture;
public final class Child<C> extends Middle<C> {
 public Child() { super();Trace.created=this; }
 @Override public boolean touch(Object value) {
  Trace.hit(1);boolean chosen=super.touch(value);Trace.hit(4);return chosen;
 }
}
