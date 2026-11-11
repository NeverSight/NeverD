package fixture;
public class Base<K,V,T> {
 protected final Object[] buffer() {
  Object[] selected=Trace.current;Trace.receiver=this;++Trace.buffers;Trace.hit(1);
  Trace.current=Trace.alternate;return selected;
 }
 protected final int index() {
  ++Trace.indexes;Trace.hit(2);int value=Trace.index;Trace.index=Trace.nextIndex;return value;
 }
}
