package fixture;
public class Base<K,V,T> {
 protected final Object select(Object value,String tag) {
  Trace.receiver=this;Trace.value=value;Trace.tag=tag;++Trace.calls;Trace.hit(1);
  return value;
 }
}
