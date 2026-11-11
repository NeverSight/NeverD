package fixture;
public class Parent<K,V> {
 static { Trace.hit(8); }
 public final K key;
 public final V value;
 public Parent(K key,V value) {
  Trace.hit(3);this.key=key;this.value=value;
  Trace.seen=this;Trace.hit(4);
 }
}
