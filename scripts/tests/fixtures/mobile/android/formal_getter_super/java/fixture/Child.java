package fixture;
import java.util.Map;
public final class Child<K,V> extends Parent<K,V> {
 static { Trace.hit(9); }
 public final Map<K,Provider<V>> map;
 public final Provider<V> receiver;
 public Child(Map<K,Provider<V>> map,K key,Provider<V> receiver) {
  super(key,receiver.fetch());this.map=map;this.receiver=receiver;Trace.hit(5);
 }
}
