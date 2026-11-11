package fixture;
import java.util.Map;
import java.util.SortedMap;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    public Core(SortedMap<? extends K,? extends V> source){super(new Fresh<K,V>((Map<? extends K,? extends V>)source));Trace.hit(4);}
    public Core(SortedMap<? extends K,? extends V> source,int marker){super(new Fresh<K,V>((Map<? extends K,? extends V>)source));Trace.hit(4);}
}
