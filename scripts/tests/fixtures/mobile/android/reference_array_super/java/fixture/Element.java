package fixture;
public final class Element<K,V> extends Path<K,V,K> {
 static { Trace.hit(8); }
 public Element() { super();Trace.hit(2);++Trace.completed; }
}
